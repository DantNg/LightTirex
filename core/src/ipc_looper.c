#include "ipc_looper.h"
#include "ipc_clock.h"

#include <stdio.h>
#include <string.h>

#ifndef IPC_MAX_LOOPERS
#define IPC_MAX_LOOPERS 12
#endif

#ifndef IPC_MAX_SYNC_TOKENS
#define IPC_MAX_SYNC_TOKENS 8
#endif

#define IPC_LOCK_TIMEOUT_MS 2000u
#define IPC_MIN_BLOCK_MS    10u

/* So sanh thoi gian an toan voi tran so tick (32-bit wrap). */
static inline bool time_reached(ipc_tick_t now, ipc_tick_t when)
{
    return (int32_t)(now - when) >= 0;
}

struct ipc_looper {
    ipc_looper_cfg_t cfg;
    char             name[20];

    ipc_mutex_t lock;
    ipc_sem_t   wake;
    ipc_task_t  task;

    ipc_message_t *head;      /* danh sach sorted theo (when_ms, seq) */
    uint32_t       pending;
    uint32_t       seq_next;

    volatile ipc_looper_state_t state;
    volatile ipc_tick_t heartbeat_ms;
    volatile bool  task_gone;
    volatile bool  quit_requested;
    volatile bool  quit_safely;

    ipc_message_t *in_flight;  /* message dang dispatch: ro ri neu task chet */
    uint32_t       tag;        /* owner_tag: index+1, on dinh suot doi looper */
    uint32_t       generation;
    uint32_t       restart_count;
    ipc_tick_t     window_start_ms;
    bool           in_use;
};

static struct ipc_looper s_loopers[IPC_MAX_LOOPERS];
static uint32_t          s_looper_count;

/* ------------------------------------------------------------------ */
/* Sync token: cho phep goi dong bo ma khong so cai sem bi huy som     */
/* ------------------------------------------------------------------ */
typedef struct {
    ipc_sem_t sem;
    volatile bool in_use;
    volatile bool abandoned; /* nguoi goi da timeout va bo di */
    volatile bool done;
} sync_token_t;

static sync_token_t s_tokens[IPC_MAX_SYNC_TOKENS];

static sync_token_t *token_acquire(void)
{
    for (int i = 0; i < IPC_MAX_SYNC_TOKENS; ++i) {
        bool taken = false;
        ipc_enter_critical();
        if (!s_tokens[i].in_use) {
            s_tokens[i].in_use = true;
            s_tokens[i].abandoned = false;
            s_tokens[i].done = false;
            taken = true;
        }
        ipc_exit_critical();
        if (taken) {
            if (!s_tokens[i].sem) s_tokens[i].sem = ipc_sem_create();
            return &s_tokens[i];
        }
    }
    return NULL;
}

static void token_release(sync_token_t *t)
{
    ipc_enter_critical();
    t->abandoned = false;
    t->done = false;
    t->in_use = false;
    ipc_exit_critical();
}

/* Goi tu phia looper sau khi xu ly xong message dong bo. */
static void token_complete(sync_token_t *t)
{
    bool orphan = false;
    ipc_enter_critical();
    t->done = true;
    orphan = t->abandoned;
    ipc_exit_critical();
    if (orphan) token_release(t);   /* nguoi goi da bo di -> ta don dep */
    else        ipc_sem_give(t->sem);
}

/* ------------------------------------------------------------------ */
/* Looper                                                              */
/* ------------------------------------------------------------------ */

void ipc_looper_cfg_default(ipc_looper_cfg_t *cfg, const char *name)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->name = name;
    cfg->stack_words = 3072;
    cfg->priority = 5;
    cfg->heartbeat_timeout_ms = 5000;
    cfg->max_restarts = 5;
    cfg->restart_window_ms = 60000;
    cfg->restart_backoff_ms = 200;
    cfg->purge_queue_on_restart = false;
}

static void looper_task_entry(void *arg);
static void queue_purge(ipc_looper_t *lp);

ipc_looper_t *ipc_looper_create(const ipc_looper_cfg_t *cfg)
{
    ipc_message_pool_init();

    ipc_looper_t *lp = NULL;
    ipc_enter_critical();
    for (int i = 0; i < IPC_MAX_LOOPERS; ++i) {
        if (!s_loopers[i].in_use) {
            lp = &s_loopers[i];
            lp->in_use = true;
            lp->tag = (uint32_t)i + 1;
            if ((uint32_t)i + 1 > s_looper_count) s_looper_count = (uint32_t)i + 1;
            break;
        }
    }
    ipc_exit_critical();
    if (!lp) return NULL;

    uint32_t tag = lp->tag;
    memset(lp, 0, sizeof(*lp));
    lp->in_use = true;
    lp->tag = tag;
    lp->cfg = *cfg;

    snprintf(lp->name, sizeof(lp->name), "%s", cfg->name ? cfg->name : "looper");
    lp->cfg.name = lp->name;

    lp->lock = ipc_mutex_create();
    lp->wake = ipc_sem_create();
    if (!lp->lock || !lp->wake) {
        if (lp->lock) ipc_mutex_destroy(lp->lock);
        if (lp->wake) ipc_sem_destroy(lp->wake);
        ipc_enter_critical();
        lp->in_use = false;
        ipc_exit_critical();
        return NULL;
    }
    lp->state = IPC_LOOPER_IDLE;
    lp->heartbeat_ms = ipc_clock_now();
    return lp;
}

void ipc_looper_destroy(ipc_looper_t *lp)
{
    if (!lp || !lp->in_use) return;
    if (lp->state == IPC_LOOPER_RUNNING) return;  /* phai quit truoc */
    queue_purge(lp);
    ipc_message_reclaim_by_owner(lp->tag);
    if (lp->lock) { ipc_mutex_destroy(lp->lock); lp->lock = NULL; }
    if (lp->wake) { ipc_sem_destroy(lp->wake);   lp->wake = NULL; }
    ipc_enter_critical();
    lp->in_use = false;
    lp->state = IPC_LOOPER_STOPPED;
    ipc_exit_critical();
}

bool ipc_looper_is_task_driven(const ipc_looper_t *lp)
{
    return lp && lp->task != NULL;
}

bool ipc_looper_restart_inplace(ipc_looper_t *lp)
{
    if (!lp || !lp->in_use) return false;
    if (lp->state == IPC_LOOPER_STOPPED) return false;

    /* Message dang dispatch dang do: thu hoi de khong ro ri pool. */
    if (lp->in_flight) {
        ipc_message_t *m = lp->in_flight;
        lp->in_flight = NULL;
        ipc_message_recycle(m);
    }
    if (lp->cfg.purge_queue_on_restart) queue_purge(lp);

    lp->quit_requested = false;
    lp->quit_safely = false;
    lp->task_gone = false;
    lp->generation++;
    lp->restart_count++;
    lp->state = IPC_LOOPER_RUNNING;
    lp->heartbeat_ms = ipc_clock_now();

    /* Dung DUNG duong on_start ma task that se chay, nen hanh vi khoi tao
     * lai duoc kiem chung y het tren board. */
    if (lp->cfg.on_start) lp->cfg.on_start(lp, lp->cfg.user);
    return true;
}

bool ipc_looper_start(ipc_looper_t *lp)
{
    if (!lp || lp->state == IPC_LOOPER_RUNNING) return false;
    lp->task_gone = false;
    lp->quit_requested = false;
    lp->quit_safely = false;
    lp->heartbeat_ms = ipc_clock_now();
    lp->state = IPC_LOOPER_RUNNING;
    if (!ipc_task_create(&lp->task, lp->name, looper_task_entry, lp,
                         lp->cfg.stack_words, lp->cfg.priority)) {
        lp->state = IPC_LOOPER_DEAD;
        return false;
    }
    return true;
}

const char *ipc_looper_name(const ipc_looper_t *lp) { return lp ? lp->name : "?"; }
ipc_looper_state_t ipc_looper_state(const ipc_looper_t *lp) { return lp ? lp->state : IPC_LOOPER_STOPPED; }
uint32_t ipc_looper_generation(const ipc_looper_t *lp) { return lp ? lp->generation : 0; }
uint32_t ipc_looper_pending(const ipc_looper_t *lp) { return lp ? lp->pending : 0; }
uint32_t ipc_looper_restart_count(const ipc_looper_t *lp) { return lp ? lp->restart_count : 0; }
uint32_t ipc_looper_count(void) { return s_looper_count; }
const ipc_looper_cfg_t *ipc_looper_cfg(const ipc_looper_t *lp) { return lp ? &lp->cfg : NULL; }

uint32_t ipc_looper_since_heartbeat_ms(const ipc_looper_t *lp)
{
    if (!lp) return 0;
    return (uint32_t)(ipc_clock_now() - lp->heartbeat_ms);
}

ipc_looper_t *ipc_looper_at(uint32_t index)
{
    if (index >= IPC_MAX_LOOPERS) return NULL;
    return s_loopers[index].in_use ? &s_loopers[index] : NULL;
}

ipc_looper_t *ipc_looper_my_looper(void) { return (ipc_looper_t *)ipc_tls_get(); }

/* ---------------- hang doi ---------------- */

/* Chen theo thu tu (when_ms, seq). Phai giu lock. */
static void queue_insert_locked(ipc_looper_t *lp, ipc_message_t *msg)
{
    msg->seq = lp->seq_next++;
    msg->owner_tag = lp->tag;
    msg->next = NULL;

    ipc_message_t **pp = &lp->head;
    while (*pp) {
        ipc_message_t *cur = *pp;
        if ((int32_t)(msg->when_ms - cur->when_ms) < 0) break;
        pp = &cur->next;
    }
    msg->next = *pp;
    *pp = msg;
    lp->pending++;
}

static bool looper_enqueue(ipc_looper_t *lp, ipc_message_t *msg, ipc_tick_t when)
{
    if (!lp || !msg) return false;
    if (lp->state == IPC_LOOPER_STOPPED || lp->quit_requested) {
        ipc_message_recycle(msg);
        return false;
    }
    msg->when_ms = when;
    if (!ipc_mutex_lock(lp->lock, IPC_LOCK_TIMEOUT_MS)) {
        /* Lock ket -> gan nhu chac chan chu so huu da chet. Khong nuot
         * message: bao that bai de tang tren xu ly / supervisor hoi sinh. */
        ipc_message_recycle(msg);
        return false;
    }
    queue_insert_locked(lp, msg);
    ipc_mutex_unlock(lp->lock);
    ipc_sem_give(lp->wake);
    return true;
}

/* Lay message da den han. NULL neu chua co; *wait_ms = thoi gian nen ngu. */
static ipc_message_t *queue_next(ipc_looper_t *lp, uint32_t max_block_ms,
                                 uint32_t *wait_ms)
{
    ipc_message_t *out = NULL;
    *wait_ms = max_block_ms;

    if (!ipc_mutex_lock(lp->lock, IPC_LOCK_TIMEOUT_MS)) {
        *wait_ms = IPC_MIN_BLOCK_MS;
        return NULL;
    }
    ipc_tick_t now = ipc_clock_now();
    ipc_message_t *m = lp->head;
    if (m) {
        if (time_reached(now, m->when_ms)) {
            lp->head = m->next;
            m->next = NULL;
            lp->pending--;
            out = m;
        } else {
            uint32_t d = (uint32_t)(m->when_ms - now);
            if (d < *wait_ms) *wait_ms = d;
        }
    }
    lp->in_flight = out;
    ipc_mutex_unlock(lp->lock);
    return out;
}

static void queue_purge(ipc_looper_t *lp)
{
    ipc_message_t *list = NULL;
    if (ipc_mutex_lock(lp->lock, IPC_LOCK_TIMEOUT_MS)) {
        list = lp->head;
        lp->head = NULL;
        lp->pending = 0;
        ipc_mutex_unlock(lp->lock);
    } else {
        /* Lock hong: doc truc tiep, chap nhan vi task so huu da chet. */
        list = lp->head;
        lp->head = NULL;
        lp->pending = 0;
    }
    while (list) {
        ipc_message_t *n = list->next;
        if (list->sync_token) token_complete((sync_token_t *)list->sync_token);
        ipc_message_recycle(list);
        list = n;
    }
}

/* ---------------- vong lap ---------------- */

static void dispatch(ipc_looper_t *lp, ipc_message_t *msg)
{
    if (msg->runnable) {
        msg->runnable(msg->payload);
    } else if (msg->target && msg->target->cb) {
        msg->target->cb(msg->target, msg, msg->target->user);
    }
    if (msg->sync_token) token_complete((sync_token_t *)msg->sync_token);
    lp->in_flight = NULL;
    ipc_message_recycle(msg);
}

void ipc_looper_run(ipc_looper_t *lp)
{
    if (!lp) return;
    ipc_tls_set(lp);
    lp->task = ipc_task_self();
    lp->state = IPC_LOOPER_RUNNING;

    uint32_t max_block = lp->cfg.heartbeat_timeout_ms
                             ? (lp->cfg.heartbeat_timeout_ms / 2)
                             : IPC_WAIT_FOREVER;
    if (max_block != IPC_WAIT_FOREVER && max_block < IPC_MIN_BLOCK_MS)
        max_block = IPC_MIN_BLOCK_MS;

    for (;;) {
        /* Nhip tim: cap nhat ca khi ban lan khi ranh. Treo trong handler
         * -> nhip dung -> supervisor phat hien. */
        lp->heartbeat_ms = ipc_clock_now();

        if (lp->quit_requested && !lp->quit_safely) break;

        uint32_t wait_ms = 0;
        ipc_message_t *msg = queue_next(lp, max_block, &wait_ms);

        if (msg) {
            dispatch(lp, msg);
            continue;
        }
        if (lp->quit_requested) break; /* quit_safely: het message den han */

        ipc_sem_take(lp->wake, wait_ms);
    }

    lp->state = IPC_LOOPER_QUITTING;
    if (lp->cfg.on_stop) lp->cfg.on_stop(lp, lp->cfg.user);
    queue_purge(lp);
    ipc_tls_set(NULL);
    lp->state = IPC_LOOPER_STOPPED;
}

uint32_t ipc_looper_poll(ipc_looper_t *lp, uint32_t max_msgs)
{
    if (!lp) return 0;
    ipc_looper_t *prev = (ipc_looper_t *)ipc_tls_get();
    ipc_tls_set(lp);
    if (lp->state == IPC_LOOPER_IDLE) lp->state = IPC_LOOPER_RUNNING;

    uint32_t n = 0;
    while (max_msgs == 0 || n < max_msgs) {
        uint32_t wait_ms;
        ipc_message_t *msg = queue_next(lp, 0, &wait_ms);
        if (!msg) break;
        lp->heartbeat_ms = ipc_clock_now();
        dispatch(lp, msg);
        n++;
    }
    lp->heartbeat_ms = ipc_clock_now();
    ipc_tls_set(prev);
    return n;
}

static void looper_task_entry(void *arg)
{
    ipc_looper_t *lp = (ipc_looper_t *)arg;

    lp->generation++;
    if (lp->cfg.on_start) lp->cfg.on_start(lp, lp->cfg.user);

    ipc_looper_run(lp);

    /* Ra toi day = thoat co trat tu. Neu task chet bat thuong, co nay
     * khong bao gio duoc dat -> heartbeat se to cao. */
    lp->task_gone = true;
    lp->task = NULL;
    ipc_task_delete(ipc_task_self());
}

void ipc_looper_quit(ipc_looper_t *lp, bool safely)
{
    if (!lp) return;
    lp->quit_safely = safely;
    lp->quit_requested = true;
    ipc_sem_give(lp->wake);
}

void ipc_looper_stop_permanently(ipc_looper_t *lp)
{
    if (!lp) return;
    ipc_looper_quit(lp, false);
    lp->state = IPC_LOOPER_STOPPED;
}

/* ---------------- phat hien chet + hoi sinh ---------------- */

bool ipc_looper_check_alive(ipc_looper_t *lp, ipc_death_reason_t *why)
{
    if (why) *why = IPC_DEATH_NONE;
    if (!lp || !lp->in_use) return true;
    if (lp->state != IPC_LOOPER_RUNNING) return true; /* IDLE/STOPPED: khong giam sat */

    if (lp->task_gone) {
        if (why) *why = IPC_DEATH_TASK_GONE;
        return false;
    }
    uint32_t timeout = lp->cfg.heartbeat_timeout_ms;
    if (timeout == 0) return true;

    ipc_tick_t now = ipc_clock_now();
    if ((uint32_t)(now - lp->heartbeat_ms) > timeout) {
        if (why) *why = IPC_DEATH_HEARTBEAT;
        return false;
    }
    return true;
}

bool ipc_looper_revive(ipc_looper_t *lp, ipc_death_reason_t why)
{
    if (!lp || lp->state == IPC_LOOPER_STOPPED) return false;

    /* 1. Cua so restart: chong crash-loop lam chet may. */
    ipc_tick_t now = ipc_clock_now();
    if (lp->cfg.restart_window_ms &&
        (uint32_t)(now - lp->window_start_ms) > lp->cfg.restart_window_ms) {
        lp->window_start_ms = now;
        lp->restart_count = 0;
    }
    if (lp->cfg.max_restarts && lp->restart_count >= lp->cfg.max_restarts) {
        lp->state = IPC_LOOPER_STOPPED;
        queue_purge(lp);
        return false;
    }
    lp->restart_count++;

    /* 2. Giet xac cu neu no con ton tai (truong hop treo, khong phai bi xoa). */
    if (why == IPC_DEATH_HEARTBEAT && lp->task && !lp->task_gone) {
        ipc_task_t victim = lp->task;
        lp->task = NULL;
        ipc_task_delete(victim);
    }
    lp->task = NULL;
    lp->task_gone = false;

    /* 3. Thu hoi message dang dispatch dang do (neu khong se ro ri pool). */
    if (lp->in_flight) {
        ipc_message_t *m = lp->in_flight;
        lp->in_flight = NULL;
        if (m->sync_token) token_complete((sync_token_t *)m->sync_token);
        ipc_message_recycle(m);
    }

    /* 4. Mutex co the da chet cung chu so huu -> tao lai neu khong lock duoc. */
    if (!ipc_mutex_lock(lp->lock, 50)) {
        ipc_mutex_t old = lp->lock;
        lp->lock = ipc_mutex_create();
        if (!lp->lock) { lp->lock = old; lp->state = IPC_LOOPER_DEAD; return false; }
        ipc_mutex_destroy(old);
    } else {
        ipc_mutex_unlock(lp->lock);
    }

    /* 5. Semaphore danh thuc: tao lai cho sach trang thai. */
    if (lp->wake) ipc_sem_destroy(lp->wake);
    lp->wake = ipc_sem_create();
    if (!lp->wake) { lp->state = IPC_LOOPER_DEAD; return false; }

    /* 6. Hang doi: giu lai (mac dinh) hoac xoa sach. */
    if (lp->cfg.purge_queue_on_restart) {
        queue_purge(lp);
        ipc_message_reclaim_by_owner(lp->tag);
    }

    if (lp->cfg.restart_backoff_ms) ipc_sleep_ms(lp->cfg.restart_backoff_ms);

    /* 7. Tao task moi gan vao DUNG object looper cu. */
    lp->quit_requested = false;
    lp->quit_safely = false;
    lp->heartbeat_ms = ipc_clock_now();
    lp->state = IPC_LOOPER_RUNNING;
    if (!ipc_task_create(&lp->task, lp->name, looper_task_entry, lp,
                         lp->cfg.stack_words, lp->cfg.priority)) {
        lp->state = IPC_LOOPER_DEAD;
        return false;
    }
    return true;
}

/* ---------------- Handler ---------------- */

void ipc_handler_init(ipc_handler_t *h, ipc_looper_t *lp, ipc_handler_cb cb,
                      void *user, const char *name)
{
    if (!h) return;
    h->looper = lp;
    h->cb = cb;
    h->user = user;
    h->name = name;
}

bool ipc_handler_send(ipc_handler_t *h, ipc_message_t *msg)
{
    return ipc_handler_send_delayed(h, msg, 0);
}

bool ipc_handler_send_delayed(ipc_handler_t *h, ipc_message_t *msg, uint32_t delay_ms)
{
    if (!h || !h->looper || !msg) {
        ipc_message_recycle(msg);
        return false;
    }
    msg->target = h;
    return looper_enqueue(h->looper, msg, ipc_clock_now() + delay_ms);
}

bool ipc_handler_send_at_front(ipc_handler_t *h, ipc_message_t *msg)
{
    if (!h || !h->looper || !msg) {
        ipc_message_recycle(msg);
        return false;
    }
    msg->target = h;
    /* Lui moc thoi gian ve qua khu -> dung dau hang doi. */
    return looper_enqueue(h->looper, msg, ipc_clock_now() - 1000u);
}

bool ipc_handler_send_empty(ipc_handler_t *h, uint32_t what)
{
    return ipc_handler_send_empty_delayed(h, what, 0);
}

bool ipc_handler_send_empty_delayed(ipc_handler_t *h, uint32_t what, uint32_t delay_ms)
{
    ipc_message_t *m = ipc_message_obtain();
    if (!m) return false;
    m->what = what;
    return ipc_handler_send_delayed(h, m, delay_ms);
}

bool ipc_handler_post(ipc_handler_t *h, ipc_runnable_fn fn, void *arg)
{
    return ipc_handler_post_delayed(h, fn, arg, 0);
}

bool ipc_handler_post_delayed(ipc_handler_t *h, ipc_runnable_fn fn, void *arg,
                              uint32_t delay_ms)
{
    ipc_message_t *m = ipc_message_obtain();
    if (!m) return false;
    m->runnable = fn;
    m->payload = arg;
    return ipc_handler_send_delayed(h, m, delay_ms);
}

bool ipc_handler_send_from_isr(ipc_handler_t *h, ipc_message_t *msg,
                               bool *higher_prio_woken)
{
    if (!h || !h->looper || !msg) return false;
    ipc_looper_t *lp = h->looper;
    if (lp->state != IPC_LOOPER_RUNNING) return false;

    msg->target = h;
    msg->when_ms = ipc_clock_now();

    /* Tu ISR khong duoc lay mutex -> dung critical section ngan. */
    ipc_enter_critical();
    queue_insert_locked(lp, msg);
    ipc_exit_critical();

    ipc_sem_give_from_isr(lp->wake, higher_prio_woken);
    return true;
}

uint32_t ipc_handler_remove(ipc_handler_t *h, uint32_t what)
{
    if (!h || !h->looper) return 0;
    ipc_looper_t *lp = h->looper;
    ipc_message_t *dead = NULL;
    uint32_t n = 0;

    if (!ipc_mutex_lock(lp->lock, IPC_LOCK_TIMEOUT_MS)) return 0;
    ipc_message_t **pp = &lp->head;
    while (*pp) {
        ipc_message_t *cur = *pp;
        if (cur->target == h && (what == IPC_WHAT_ANY || cur->what == what)) {
            *pp = cur->next;
            cur->next = dead;
            dead = cur;
            lp->pending--;
            n++;
        } else {
            pp = &cur->next;
        }
    }
    ipc_mutex_unlock(lp->lock);

    while (dead) {
        ipc_message_t *nx = dead->next;
        if (dead->sync_token) token_complete((sync_token_t *)dead->sync_token);
        ipc_message_recycle(dead);
        dead = nx;
    }
    return n;
}

bool ipc_handler_has(ipc_handler_t *h, uint32_t what)
{
    if (!h || !h->looper) return false;
    ipc_looper_t *lp = h->looper;
    bool found = false;
    if (!ipc_mutex_lock(lp->lock, IPC_LOCK_TIMEOUT_MS)) return false;
    for (ipc_message_t *m = lp->head; m; m = m->next) {
        if (m->target == h && (what == IPC_WHAT_ANY || m->what == what)) {
            found = true;
            break;
        }
    }
    ipc_mutex_unlock(lp->lock);
    return found;
}

bool ipc_handler_send_sync(ipc_handler_t *h, uint32_t what, int32_t arg1,
                           void *payload, uint32_t timeout_ms)
{
    if (!h || !h->looper) return false;
    /* Goi dong bo tu chinh looper dich = tu khoa chan minh. */
    if (ipc_looper_my_looper() == h->looper) return false;

    sync_token_t *tok = token_acquire();
    if (!tok) return false;

    ipc_message_t *m = ipc_message_obtain();
    if (!m) { token_release(tok); return false; }
    m->what = what;
    m->arg1 = arg1;
    m->payload = payload;
    m->sync_token = tok;

    if (!ipc_handler_send(h, m)) { token_release(tok); return false; }

    if (ipc_sem_take(tok->sem, timeout_ms)) {
        token_release(tok);
        return true;
    }
    /* Timeout: co the task dich vua chet. Bo token lai cho ben kia don. */
    bool already_done;
    ipc_enter_critical();
    already_done = tok->done;
    if (!already_done) tok->abandoned = true;
    ipc_exit_critical();
    if (already_done) token_release(tok);
    return false;
}
