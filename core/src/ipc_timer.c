#include "ipc_timer.h"
#include "ipc_clock.h"
#include "ipc_service.h"

#include <stdio.h>
#include <string.h>

#define TMR_LOCK_MS 1000u
/* Chan tren so timer xu ly trong mot nhip: khong de mot vong lap tre lam
 * dong bang engine. Phan con lai se duoc ban o nhip ke tiep. */
#define TMR_MAX_FIRE_PER_STEP 12
/* Chan tren so nhip bu khi khong coalesce, tranh vong lap dai vo han. */
#define TMR_MAX_CATCHUP 64

typedef struct tmr {
    ipc_timer_cfg_t cfg;
    char name[IPC_TIMER_NAME_LEN];
    char service[IPC_TIMER_NAME_LEN];

    ipc_tick_t deadline;
    bool     in_use;
    bool     active;
    uint16_t gen;
    uint16_t slot;

    ipc_timer_stats_t st;
    struct tmr *next;     /* trong danh sach active, sorted theo deadline */
} tmr_t;

static tmr_t      s_timers[IPC_MAX_TIMERS];
static tmr_t     *s_active;          /* danh sach sorted */
static ipc_mutex_t s_lock;
static ipc_sem_t   s_wake;
static ipc_task_t  s_task;
static volatile bool s_running;

static inline bool reached(ipc_tick_t now, ipc_tick_t when)
{
    return (int32_t)(now - when) >= 0;
}

static bool ensure_init(void)
{
    if (s_lock) return true;
    ipc_message_pool_init();
    s_lock = ipc_mutex_create();
    s_wake = ipc_sem_create();
    return s_lock && s_wake;
}

/* ---------------- id <-> slot ---------------- */

static ipc_timer_id_t make_id(const tmr_t *t)
{
    return ((ipc_timer_id_t)t->gen << 16) | (ipc_timer_id_t)(t->slot + 1);
}

/* Phai giu lock. */
static tmr_t *resolve(ipc_timer_id_t id)
{
    if (id == IPC_TIMER_NONE) return NULL;
    uint16_t slot = (uint16_t)((id & 0xFFFFu) - 1);
    uint16_t gen  = (uint16_t)(id >> 16);
    if (slot >= IPC_MAX_TIMERS) return NULL;
    tmr_t *t = &s_timers[slot];
    if (!t->in_use || t->gen != gen) return NULL;  /* id cu cua slot da tai su dung */
    return t;
}

/* ---------------- danh sach active (phai giu lock) ---------------- */

static void list_remove(tmr_t *t)
{
    tmr_t **pp = &s_active;
    while (*pp) {
        if (*pp == t) { *pp = t->next; t->next = NULL; t->active = false; return; }
        pp = &(*pp)->next;
    }
    t->active = false;
}

static void list_insert(tmr_t *t)
{
    tmr_t **pp = &s_active;
    while (*pp && (int32_t)(t->deadline - (*pp)->deadline) >= 0)
        pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;
    t->active = true;
}

static void arm_locked(tmr_t *t, uint32_t delay_ms)
{
    if (t->active) list_remove(t);
    t->deadline = ipc_clock_now() + delay_ms;
    list_insert(t);
}

/* ---------------- cau hinh ---------------- */

void ipc_timer_cfg_default(ipc_timer_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = IPC_TIMER_ONESHOT;
    cfg->delivery = IPC_TIMER_TO_HANDLER;
    cfg->coalesce_missed = true;
    cfg->auto_start = true;
}

void ipc_timer_engine_cfg_default(ipc_timer_engine_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->priority = 15;
    cfg->stack_words = 3072;
    cfg->own_task = true;
}

/* ---------------- tao/huy ---------------- */

ipc_timer_id_t ipc_timer_create(const ipc_timer_cfg_t *cfg)
{
    if (!cfg || !ensure_init()) return IPC_TIMER_NONE;

    /* Kiem tra cau hinh som: sai cau hinh phai bao ngay, khong im lang. */
    switch (cfg->delivery) {
    case IPC_TIMER_TO_HANDLER:  if (!cfg->handler)  return IPC_TIMER_NONE; break;
    case IPC_TIMER_TO_SERVICE:  if (!cfg->service)  return IPC_TIMER_NONE; break;
    case IPC_TIMER_TO_CALLBACK: if (!cfg->callback) return IPC_TIMER_NONE; break;
    default: return IPC_TIMER_NONE;
    }
    if (cfg->mode == IPC_TIMER_PERIODIC &&
        cfg->period_ms == 0 && cfg->delay_ms == 0)
        return IPC_TIMER_NONE;   /* chu ky 0 = vong lap ban vo tan */

    if (!ipc_mutex_lock(s_lock, TMR_LOCK_MS)) return IPC_TIMER_NONE;

    tmr_t *t = NULL;
    for (int i = 0; i < IPC_MAX_TIMERS; ++i) {
        if (!s_timers[i].in_use) { t = &s_timers[i]; t->slot = (uint16_t)i; break; }
    }
    if (!t) { ipc_mutex_unlock(s_lock); return IPC_TIMER_NONE; }

    uint16_t gen = (uint16_t)(t->gen + 1);
    uint16_t slot = t->slot;
    memset(t, 0, sizeof(*t));
    t->gen = gen;
    t->slot = slot;
    t->in_use = true;
    t->cfg = *cfg;

    snprintf(t->name, sizeof(t->name), "%s", cfg->name ? cfg->name : "tmr");
    t->cfg.name = t->name;
    if (cfg->service) {
        snprintf(t->service, sizeof(t->service), "%s", cfg->service);
        t->cfg.service = t->service;
    }

    if (cfg->auto_start) arm_locked(t, cfg->delay_ms);
    ipc_timer_id_t id = make_id(t);
    ipc_mutex_unlock(s_lock);

    if (cfg->auto_start && s_wake) ipc_sem_give(s_wake);
    return id;
}

void ipc_timer_destroy(ipc_timer_id_t id)
{
    if (!s_lock) return;
    if (!ipc_mutex_lock(s_lock, TMR_LOCK_MS)) return;
    tmr_t *t = resolve(id);
    if (t) {
        if (t->active) list_remove(t);
        t->in_use = false;   /* gen giu nguyen; lan cap phat sau se tang */
    }
    ipc_mutex_unlock(s_lock);
}

/* ---------------- dieu khien ---------------- */

bool ipc_timer_start(ipc_timer_id_t id)
{
    if (!s_lock || !ipc_mutex_lock(s_lock, TMR_LOCK_MS)) return false;
    tmr_t *t = resolve(id);
    bool ok = false;
    if (t) { arm_locked(t, t->cfg.delay_ms); ok = true; }
    ipc_mutex_unlock(s_lock);
    if (ok) ipc_sem_give(s_wake);
    return ok;
}

bool ipc_timer_stop(ipc_timer_id_t id)
{
    if (!s_lock || !ipc_mutex_lock(s_lock, TMR_LOCK_MS)) return false;
    tmr_t *t = resolve(id);
    bool ok = false;
    if (t) { if (t->active) list_remove(t); ok = true; }
    ipc_mutex_unlock(s_lock);
    return ok;
}

bool ipc_timer_restart(ipc_timer_id_t id, uint32_t new_delay_ms)
{
    if (!s_lock || !ipc_mutex_lock(s_lock, TMR_LOCK_MS)) return false;
    tmr_t *t = resolve(id);
    bool ok = false;
    if (t) {
        if (new_delay_ms) t->cfg.delay_ms = new_delay_ms;
        arm_locked(t, t->cfg.delay_ms);
        ok = true;
    }
    ipc_mutex_unlock(s_lock);
    if (ok) ipc_sem_give(s_wake);
    return ok;
}

bool ipc_timer_set_period(ipc_timer_id_t id, uint32_t period_ms)
{
    if (!s_lock || !ipc_mutex_lock(s_lock, TMR_LOCK_MS)) return false;
    tmr_t *t = resolve(id);
    bool ok = false;
    if (t && period_ms) { t->cfg.period_ms = period_ms; ok = true; }
    ipc_mutex_unlock(s_lock);
    return ok;
}

bool ipc_timer_is_active(ipc_timer_id_t id)
{
    if (!s_lock || !ipc_mutex_lock(s_lock, TMR_LOCK_MS)) return false;
    tmr_t *t = resolve(id);
    bool a = t && t->active;
    ipc_mutex_unlock(s_lock);
    return a;
}

uint32_t ipc_timer_remaining_ms(ipc_timer_id_t id)
{
    if (!s_lock || !ipc_mutex_lock(s_lock, TMR_LOCK_MS)) return 0;
    tmr_t *t = resolve(id);
    uint32_t r = 0;
    if (t && t->active) {
        ipc_tick_t now = ipc_clock_now();
        r = reached(now, t->deadline) ? 0 : (uint32_t)(t->deadline - now);
    }
    ipc_mutex_unlock(s_lock);
    return r;
}

static uint32_t cancel_where(ipc_handler_t *h, const char *service)
{
    if (!s_lock || !ipc_mutex_lock(s_lock, TMR_LOCK_MS)) return 0;
    uint32_t n = 0;
    for (int i = 0; i < IPC_MAX_TIMERS; ++i) {
        tmr_t *t = &s_timers[i];
        if (!t->in_use) continue;
        bool hit = (h && t->cfg.delivery == IPC_TIMER_TO_HANDLER && t->cfg.handler == h) ||
                   (service && t->cfg.delivery == IPC_TIMER_TO_SERVICE &&
                    strncmp(t->service, service, IPC_TIMER_NAME_LEN) == 0);
        if (hit) {
            if (t->active) list_remove(t);
            t->in_use = false;
            n++;
        }
    }
    ipc_mutex_unlock(s_lock);
    return n;
}

uint32_t ipc_timer_cancel_for_handler(ipc_handler_t *h) { return cancel_where(h, NULL); }
uint32_t ipc_timer_cancel_for_service(const char *s)    { return cancel_where(NULL, s); }

bool ipc_timer_stats(ipc_timer_id_t id, ipc_timer_stats_t *out)
{
    if (!out || !s_lock || !ipc_mutex_lock(s_lock, TMR_LOCK_MS)) return false;
    tmr_t *t = resolve(id);
    if (t) *out = t->st;
    bool ok = (t != NULL);
    ipc_mutex_unlock(s_lock);
    return ok;
}

void ipc_timer_dump(void (*print)(const char *line))
{
    if (!print || !s_lock || !ipc_mutex_lock(s_lock, TMR_LOCK_MS)) return;
    char line[128];
    ipc_tick_t now = ipc_clock_now();
    for (int i = 0; i < IPC_MAX_TIMERS; ++i) {
        tmr_t *t = &s_timers[i];
        if (!t->in_use) continue;
        snprintf(line, sizeof(line),
                 "tmr[%2d] %-12s %s %s in=%ldms fired=%u missed=%u drop=%u late=%ums",
                 i, t->name,
                 t->cfg.mode == IPC_TIMER_PERIODIC ? "per" : "one",
                 t->active ? "ON " : "off",
                 t->active ? (long)(int32_t)(t->deadline - now) : 0L,
                 (unsigned)t->st.fired, (unsigned)t->st.missed,
                 (unsigned)t->st.dropped, (unsigned)t->st.max_lateness_ms);
        print(line);
    }
    ipc_mutex_unlock(s_lock);
}

/* ---------------- ban su kien ---------------- */

/* Chay NGOAI vung khoa engine. */
static bool deliver(tmr_t *t)
{
    switch (t->cfg.delivery) {
    case IPC_TIMER_TO_CALLBACK:
        t->cfg.callback(t->cfg.callback_arg);
        return true;

    case IPC_TIMER_TO_HANDLER:
    case IPC_TIMER_TO_SERVICE: {
        ipc_handler_t *h = (t->cfg.delivery == IPC_TIMER_TO_HANDLER)
                               ? t->cfg.handler
                               /* Phan giai lai moi lan no: dich vu vua hoi sinh
                                * van nhan duoc, con tro cu khong con dung. */
                               : ipc_service_get(t->service);
        if (!h) return false;

        ipc_message_t *m = ipc_message_obtain();
        if (!m) return false;          /* pool can -> tinh la dropped */
        m->what = t->cfg.what;
        m->arg1 = t->cfg.arg1;
        m->arg2 = t->cfg.arg2;
        m->payload = t->cfg.payload;
        /* Chi ONESHOT moi duoc so huu payload; PERIODIC dung chung payload
         * qua nhieu lan ban nen khong duoc giai phong. */
        if (t->cfg.mode == IPC_TIMER_ONESHOT) m->payload_free = t->cfg.payload_free;
        return ipc_handler_send(h, m);
    }
    default:
        return false;
    }
}

uint32_t ipc_timer_step(uint32_t *next_delay_ms)
{
    if (next_delay_ms) *next_delay_ms = IPC_WAIT_FOREVER;
    if (!s_lock) return 0;
    if (!ipc_mutex_lock(s_lock, TMR_LOCK_MS)) {
        if (next_delay_ms) *next_delay_ms = 10;
        return 0;
    }

    ipc_tick_t now = ipc_clock_now();

    /* 1. Go cac timer den han ra khoi danh sach (van trong vung khoa). */
    tmr_t *fire = NULL, **ftail = &fire;
    int taken = 0;
    while (s_active && reached(now, s_active->deadline) && taken < TMR_MAX_FIRE_PER_STEP) {
        tmr_t *t = s_active;
        s_active = t->next;
        t->next = NULL;
        t->active = false;
        *ftail = t;
        ftail = &t->next;
        taken++;
    }
    ipc_mutex_unlock(s_lock);

    /* 2. Ban ngoai vung khoa: callback nguoi dung khong bao gio chay khi ta
     *    dang giu lock engine -> khong the deadlock nguoc lai vao timer API. */
    uint32_t fired = 0;
    while (fire) {
        tmr_t *t = fire;
        fire = t->next;
        t->next = NULL;

        uint32_t late = (uint32_t)(now - t->deadline);
        bool ok = deliver(t);

        if (!ipc_mutex_lock(s_lock, TMR_LOCK_MS)) continue;
        if (!t->in_use) { ipc_mutex_unlock(s_lock); continue; } /* bi huy khi dang ban */

        if (ok) { t->st.fired++; fired++; }
        else    { t->st.dropped++; }
        if (late > t->st.max_lateness_ms) t->st.max_lateness_ms = late;

        if (t->cfg.mode == IPC_TIMER_PERIODIC) {
            uint32_t period = t->cfg.period_ms ? t->cfg.period_ms : t->cfg.delay_ms;
            ipc_tick_t next = t->deadline + period;
            if (t->cfg.coalesce_missed) {
                if (reached(now, next)) {
                    /* Tre nhieu chu ky: bo qua phan da lo, can gio lai tu bay gio. */
                    t->st.missed += (late / (period ? period : 1));
                    next = now + period;
                }
            } else {
                int guard = 0;
                while (reached(now, next) && guard++ < TMR_MAX_CATCHUP) {
                    t->st.missed++;
                    next += period;
                }
            }
            t->deadline = next;
            list_insert(t);
        } else {
            t->in_use = false;   /* oneshot: tu thu hoi slot */
        }
        ipc_mutex_unlock(s_lock);
    }

    /* 3. Tinh thoi diem thuc day ke tiep. */
    if (next_delay_ms) {
        if (!ipc_mutex_lock(s_lock, TMR_LOCK_MS)) { *next_delay_ms = 10; return fired; }
        if (s_active) {
            ipc_tick_t n2 = ipc_clock_now();
            *next_delay_ms = reached(n2, s_active->deadline)
                                 ? 0
                                 : (uint32_t)(s_active->deadline - n2);
        }
        ipc_mutex_unlock(s_lock);
    }
    return fired;
}

/* ---------------- engine ---------------- */

static void timer_task(void *arg)
{
    (void)arg;
    while (s_running) {
        uint32_t next = IPC_WAIT_FOREVER;
        ipc_timer_step(&next);
        /* Chan tren de con phan ung voi engine_stop() va voi timer moi duoc
         * them tu ISR (noi khong the give semaphore an toan moi luc). */
        if (next == IPC_WAIT_FOREVER || next > 1000) next = 1000;
        if (next) ipc_sem_take(s_wake, next);
    }
    s_task = NULL;
    ipc_task_delete(ipc_task_self());
}

bool ipc_timer_engine_start(const ipc_timer_engine_cfg_t *cfg)
{
    if (!ensure_init()) return false;
    ipc_timer_engine_cfg_t c;
    if (cfg) c = *cfg;
    else ipc_timer_engine_cfg_default(&c);

    if (!c.own_task) return true;   /* che do test: nguoi dung tu goi step() */
    if (s_running) return true;

    s_running = true;
    if (!ipc_task_create(&s_task, "ipc_timer", timer_task, NULL,
                         c.stack_words ? c.stack_words : 3072, c.priority)) {
        s_running = false;
        return false;
    }
    return true;
}

void ipc_timer_engine_stop(void)
{
    s_running = false;
    if (s_wake) ipc_sem_give(s_wake);
}

/* ---------------- duong tat ---------------- */

ipc_timer_id_t ipc_timer_send_delayed(ipc_handler_t *h, uint32_t what,
                                      int32_t arg1, uint32_t delay_ms)
{
    ipc_timer_cfg_t c;
    ipc_timer_cfg_default(&c);
    c.name = "delayed";
    c.mode = IPC_TIMER_ONESHOT;
    c.delivery = IPC_TIMER_TO_HANDLER;
    c.handler = h;
    c.what = what;
    c.arg1 = arg1;
    c.delay_ms = delay_ms;
    return ipc_timer_create(&c);
}

ipc_timer_id_t ipc_timer_send_periodic(ipc_handler_t *h, uint32_t what,
                                       uint32_t period_ms)
{
    ipc_timer_cfg_t c;
    ipc_timer_cfg_default(&c);
    c.name = "periodic";
    c.mode = IPC_TIMER_PERIODIC;
    c.delivery = IPC_TIMER_TO_HANDLER;
    c.handler = h;
    c.what = what;
    c.delay_ms = period_ms;
    c.period_ms = period_ms;
    return ipc_timer_create(&c);
}

ipc_timer_id_t ipc_timer_send_periodic_to(const char *service, uint32_t what,
                                          uint32_t period_ms)
{
    ipc_timer_cfg_t c;
    ipc_timer_cfg_default(&c);
    c.name = service;
    c.mode = IPC_TIMER_PERIODIC;
    c.delivery = IPC_TIMER_TO_SERVICE;
    c.service = service;
    c.what = what;
    c.delay_ms = period_ms;
    c.period_ms = period_ms;
    return ipc_timer_create(&c);
}

ipc_timer_id_t ipc_timer_call_after(ipc_runnable_fn fn, void *arg, uint32_t delay_ms)
{
    ipc_timer_cfg_t c;
    ipc_timer_cfg_default(&c);
    c.name = "call";
    c.mode = IPC_TIMER_ONESHOT;
    c.delivery = IPC_TIMER_TO_CALLBACK;
    c.callback = fn;
    c.callback_arg = arg;
    c.delay_ms = delay_ms;
    return ipc_timer_create(&c);
}
