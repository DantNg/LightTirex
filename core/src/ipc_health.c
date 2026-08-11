#include "ipc_health.h"
#include "ipc_clock.h"
#include "ipc_service.h"

#include <stdio.h>
#include <string.h>

#define HEALTH_LOCK_MS 1000u

typedef struct {
    char     source[IPC_HEALTH_NAME_LEN];
    uint32_t code;
    ipc_severity_t sev;
    int32_t  detail;
    ipc_tick_t when;
} report_t;

/* Bo dem so lan khop cua tung (nguon, luat) de ap nguong theo cua so. */
typedef struct {
    char      source[IPC_HEALTH_NAME_LEN];
    uint8_t   rule_index;
    uint32_t  hits;
    ipc_tick_t window_start;
    bool      in_use;
} counter_t;

static report_t  s_ring[IPC_HEALTH_RING_SIZE];
static volatile uint32_t s_ring_head, s_ring_tail, s_ring_dropped;

static ipc_health_rule_t s_rules[IPC_HEALTH_MAX_RULES];
static uint32_t          s_rule_count;
static counter_t         s_counters[IPC_HEALTH_MAX_SOURCES];

static ipc_health_cfg_t   s_cfg;
static ipc_health_probe_t *s_probe;
static ipc_mutex_t  s_lock;
static ipc_task_t   s_task;
static volatile bool s_running;
static ipc_tick_t   s_boot_ms;
static ipc_health_status_t s_st;

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
static uint32_t plat_free(ipc_health_probe_t *s)
{ (void)s; return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT); }
static uint32_t plat_min_free(ipc_health_probe_t *s)
{ (void)s; return (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT); }
#else
/* Desktop hoac FreeRTOS thuan: khong co so lieu heap dang tin -> tra 0 va
 * cac nguong heap se bi bo qua (xem check_heap). */
static uint32_t plat_free(ipc_health_probe_t *s)     { (void)s; return 0; }
static uint32_t plat_min_free(ipc_health_probe_t *s) { (void)s; return 0; }
#endif

static ipc_health_probe_t s_plat_probe = {
    .name = "platform",
    .free_heap = plat_free,
    .min_free_heap = plat_min_free,
    .impl = 0,
};

ipc_health_probe_t *ipc_health_probe_platform(void) { return &s_plat_probe; }

static uint32_t fake_free(ipc_health_probe_t *s)
{ return ((ipc_health_fake_probe_t *)s->impl)->heap; }
static uint32_t fake_min_free(ipc_health_probe_t *s)
{ return ((ipc_health_fake_probe_t *)s->impl)->min_heap; }

void ipc_health_fake_probe_init(ipc_health_fake_probe_t *fp, uint32_t heap)
{
    memset(fp, 0, sizeof(*fp));
    fp->heap = heap;
    fp->min_heap = heap;
    fp->base.name = "fake";
    fp->base.free_heap = fake_free;
    fp->base.min_free_heap = fake_min_free;
    fp->base.impl = fp;
}

/* ------------------------------------------------------------------ */
/* Cau hinh + luat                                                     */
/* ------------------------------------------------------------------ */

void ipc_health_cfg_default(ipc_health_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->probe = ipc_health_probe_platform();
    cfg->backend = ipc_wdt_backend_noop();
    cfg->heap_warn_bytes = 0;
    cfg->heap_critical_bytes = 0;
    cfg->queue_depth_warn = 32;
    cfg->pool_free_warn = 4;
    cfg->check_interval_ms = 1000;
    cfg->priority = 18;
    cfg->stack_words = 3072;
    cfg->own_task = true;
}

bool ipc_health_add_rule(const ipc_health_rule_t *rule)
{
    if (!rule || s_rule_count >= IPC_HEALTH_MAX_RULES) return false;
    s_rules[s_rule_count++] = *rule;
    return true;
}

void ipc_health_clear_rules(void)
{
    s_rule_count = 0;
    memset(s_counters, 0, sizeof(s_counters));
}

/* ------------------------------------------------------------------ */
/* Bao cao (vong dem)                                                  */
/* ------------------------------------------------------------------ */

void ipc_health_report(const char *source, uint32_t code, ipc_severity_t sev,
                       int32_t detail)
{
    ipc_enter_critical();
    uint32_t next = (s_ring_head + 1) % IPC_HEALTH_RING_SIZE;
    if (next == s_ring_tail) {
        /* Day: bo bao cao MOI chu khong ghi de bao cao cu. Bao cao dau tien
         * cua mot su co thuong co gia tri chan doan cao nhat. */
        s_ring_dropped++;
        ipc_exit_critical();
        return;
    }
    report_t *r = &s_ring[s_ring_head];
    snprintf(r->source, sizeof(r->source), "%s", source ? source : "?");
    r->code = code;
    r->sev = sev;
    r->detail = detail;
    r->when = ipc_clock_now();
    s_ring_head = next;
    ipc_exit_critical();
}

static bool ring_pop(report_t *out)
{
    bool got = false;
    ipc_enter_critical();
    if (s_ring_tail != s_ring_head) {
        *out = s_ring[s_ring_tail];
        s_ring_tail = (s_ring_tail + 1) % IPC_HEALTH_RING_SIZE;
        got = true;
    }
    ipc_exit_critical();
    return got;
}

/* ------------------------------------------------------------------ */
/* Thi hanh                                                            */
/* ------------------------------------------------------------------ */

static ipc_looper_t *looper_of(const char *service)
{
    ipc_handler_t *h = ipc_service_get(service);
    return h ? h->looper : NULL;
}

static void execute(const report_t *r, ipc_health_action_t act)
{
    ipc_looper_t *lp = looper_of(r->source);
    bool handled = false;

    switch (act) {
    case IPC_ACT_NONE:
        return;

    case IPC_ACT_LOG:
        handled = true;
        break;

    case IPC_ACT_RESTART_SERVICE:
        if (s_cfg.recovery) handled = s_cfg.recovery(r->source, lp, s_cfg.user);
        if (handled) s_st.restarts++;
        break;

    case IPC_ACT_KILL_SERVICE:
        /* Giet han: dung looper vinh vien de supervisor KHONG hoi sinh no,
         * va rut dang ky dich vu de khong ai con gui viec vao ho den. */
        if (lp) {
            ipc_looper_stop_permanently(lp);
            handled = true;
        }
        ipc_service_unregister(r->source);
        break;

    case IPC_ACT_SAFE_MODE:
        if (s_cfg.on_safe_mode) { s_cfg.on_safe_mode(s_cfg.user); handled = true; }
        break;

    case IPC_ACT_REBOOT:
        s_st.reboots++;
        if (s_cfg.on_event)
            s_cfg.on_event(r->source, r->code, r->sev, act, r->detail, s_cfg.user);
        if (s_cfg.backend && s_cfg.backend->reset_system)
            s_cfg.backend->reset_system(s_cfg.backend);
        return;   /* tren phan cung that: khong tro ve toi day */
    }

    s_st.actions_taken++;
    s_st.last_action = act;
    snprintf(s_st.last_source, sizeof(s_st.last_source), "%s", r->source);
    if (act != IPC_ACT_LOG || r->sev >= IPC_SEV_ERROR) s_st.degraded = true;

    if (s_cfg.on_event)
        s_cfg.on_event(r->source, r->code, r->sev, act, r->detail, s_cfg.user);
    (void)handled;
}

/* Tim bo dem cho cap (nguon, luat). NULL neu het slot. */
static counter_t *counter_for(const char *source, uint8_t rule_index)
{
    counter_t *free_slot = NULL;
    for (int i = 0; i < IPC_HEALTH_MAX_SOURCES; ++i) {
        counter_t *c = &s_counters[i];
        if (!c->in_use) { if (!free_slot) free_slot = c; continue; }
        if (c->rule_index == rule_index &&
            strncmp(c->source, source, IPC_HEALTH_NAME_LEN) == 0)
            return c;
    }
    if (!free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    snprintf(free_slot->source, sizeof(free_slot->source), "%s", source);
    free_slot->rule_index = rule_index;
    free_slot->window_start = ipc_clock_now();
    free_slot->in_use = true;
    return free_slot;
}

static bool rule_matches(const ipc_health_rule_t *ru, const report_t *r)
{
    if (ru->code != IPC_EXC_ANY && ru->code != r->code) return false;
    if (ru->source && strncmp(ru->source, r->source, IPC_HEALTH_NAME_LEN) != 0)
        return false;
    return r->sev >= ru->min_severity;
}

/* Tra ve hanh dong can lam, hoac IPC_ACT_NONE. */
static ipc_health_action_t evaluate(const report_t *r)
{
    for (uint32_t i = 0; i < s_rule_count; ++i) {
        ipc_health_rule_t *ru = &s_rules[i];
        if (!rule_matches(ru, r)) continue;

        uint32_t need = ru->count ? ru->count : 1;
        if (need == 1) return ru->action;   /* khong can dem */

        counter_t *c = counter_for(r->source, (uint8_t)i);
        if (!c) return ru->action;          /* het slot dem -> hanh dong ngay */

        ipc_tick_t now = ipc_clock_now();
        if (ru->window_ms && (uint32_t)(now - c->window_start) > ru->window_ms) {
            c->window_start = now;          /* cua so cu het han -> dem lai */
            c->hits = 0;
        }
        c->hits++;
        if (c->hits >= need) {
            c->hits = 0;
            c->window_start = now;
            return ru->action;
        }
        return IPC_ACT_NONE;   /* luat dau tien khop da quyet dinh: chua du nguong */
    }
    return IPC_ACT_NONE;
}

/* ------------------------------------------------------------------ */
/* Quet dinh ky                                                        */
/* ------------------------------------------------------------------ */

static void scan_resources(void)
{
    /* Heap. Probe tra 0 nghia la nen tang khong cung cap so lieu -> bo qua,
     * khong bao dong gia. */
    uint32_t heap = s_probe->free_heap ? s_probe->free_heap(s_probe) : 0;
    s_st.free_heap = heap;
    s_st.min_free_heap = s_probe->min_free_heap ? s_probe->min_free_heap(s_probe) : 0;

    if (heap > 0) {
        if (s_cfg.heap_critical_bytes && heap < s_cfg.heap_critical_bytes)
            ipc_health_report("heap", IPC_EXC_LOW_HEAP, IPC_SEV_ERROR, (int32_t)heap);
        else if (s_cfg.heap_warn_bytes && heap < s_cfg.heap_warn_bytes)
            ipc_health_report("heap", IPC_EXC_LOW_HEAP, IPC_SEV_WARN, (int32_t)heap);
    }

    /* Pool message: can pool = moi thu ngung chay, phai bao som. */
    uint32_t pool_free = 0, low = 0;
    ipc_message_pool_stats(&pool_free, &low);
    s_st.pool_free = pool_free;
    if (s_cfg.pool_free_warn && pool_free < s_cfg.pool_free_warn)
        ipc_health_report("msgpool", IPC_EXC_OOM, IPC_SEV_ERROR, (int32_t)pool_free);

    /* Hang doi looper qua sau = nguoi tieu thu khong theo kip nguoi san xuat. */
    if (s_cfg.queue_depth_warn) {
        uint32_t n = ipc_looper_count();
        for (uint32_t i = 0; i < n; ++i) {
            ipc_looper_t *lp = ipc_looper_at(i);
            if (!lp || ipc_looper_state(lp) != IPC_LOOPER_RUNNING) continue;
            uint32_t depth = ipc_looper_pending(lp);
            if (depth > s_cfg.queue_depth_warn)
                ipc_health_report(ipc_looper_name(lp), IPC_EXC_QUEUE_FULL,
                                  IPC_SEV_WARN, (int32_t)depth);
        }
    }
}

uint32_t ipc_health_check(void)
{
    if (!s_probe) return 0;
    if (s_lock && !ipc_mutex_lock(s_lock, HEALTH_LOCK_MS)) return 0;

    scan_resources();

    uint32_t acted = 0;
    report_t r;
    /* Rut het vong dem. scan_resources() vua co the them bao cao vao day nen
     * thu tu la: quet truoc, rut sau - moi thu duoc xu ly trong cung mot nhip. */
    while (ring_pop(&r)) {
        if (r.sev <= IPC_SEV_FATAL) s_st.exceptions[r.sev]++;
        ipc_health_action_t act = evaluate(&r);
        if (act != IPC_ACT_NONE) {
            execute(&r, act);
            acted++;
        } else if (s_cfg.on_event && r.sev >= IPC_SEV_WARN) {
            /* Khong co luat nao ap dung: van bao ra ngoai de con nhin thay. */
            s_cfg.on_event(r.source, r.code, r.sev, IPC_ACT_NONE, r.detail, s_cfg.user);
        }
    }
    s_st.dropped_reports = s_ring_dropped;
    s_st.uptime_ms = (uint32_t)(ipc_clock_now() - s_boot_ms);

    if (s_lock) ipc_mutex_unlock(s_lock);
    return acted;
}

static void health_task(void *arg)
{
    (void)arg;
    while (s_running) {
        ipc_health_check();
        ipc_sleep_ms(s_cfg.check_interval_ms);
    }
    s_task = NULL;
    ipc_task_delete(ipc_task_self());
}

bool ipc_health_start(const ipc_health_cfg_t *cfg)
{
    if (!s_lock) s_lock = ipc_mutex_create();
    if (cfg) s_cfg = *cfg;
    else ipc_health_cfg_default(&s_cfg);
    if (s_cfg.check_interval_ms == 0) s_cfg.check_interval_ms = 1000;
    if (s_cfg.stack_words == 0) s_cfg.stack_words = 3072;

    s_probe = s_cfg.probe ? s_cfg.probe : ipc_health_probe_platform();
    if (!s_cfg.backend) s_cfg.backend = ipc_wdt_backend_noop();
    s_boot_ms = ipc_clock_now();
    memset(&s_st, 0, sizeof(s_st));

    if (!s_cfg.own_task) return true;   /* che do test */
    if (s_running) return true;

    s_running = true;
    if (!ipc_task_create(&s_task, "ipc_health", health_task, NULL,
                         s_cfg.stack_words, s_cfg.priority)) {
        s_running = false;
        return false;
    }
    return true;
}

void ipc_health_stop(void) { s_running = false; }

void ipc_health_get_status(ipc_health_status_t *out)
{
    if (!out) return;
    if (s_lock && ipc_mutex_lock(s_lock, HEALTH_LOCK_MS)) {
        *out = s_st;
        ipc_mutex_unlock(s_lock);
    } else {
        *out = s_st;
    }
}

void ipc_health_clear_degraded(void) { s_st.degraded = false; }

void ipc_health_dump(void (*print)(const char *line))
{
    if (!print) return;
    char line[160];
    ipc_health_status_t st;
    ipc_health_get_status(&st);
    snprintf(line, sizeof(line),
             "health: up=%us heap=%u/min=%u pool=%u info/warn/err/fatal=%u/%u/%u/%u",
             (unsigned)(st.uptime_ms / 1000), (unsigned)st.free_heap,
             (unsigned)st.min_free_heap, (unsigned)st.pool_free,
             (unsigned)st.exceptions[0], (unsigned)st.exceptions[1],
             (unsigned)st.exceptions[2], (unsigned)st.exceptions[3]);
    print(line);
    snprintf(line, sizeof(line),
             "health: actions=%u restarts=%u reboots=%u dropped=%u last=%s/%d %s",
             (unsigned)st.actions_taken, (unsigned)st.restarts,
             (unsigned)st.reboots, (unsigned)st.dropped_reports,
             st.last_source[0] ? st.last_source : "-", (int)st.last_action,
             st.degraded ? "DEGRADED" : "ok");
    print(line);
}
