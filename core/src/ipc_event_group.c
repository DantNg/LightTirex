#include "ipc_event_group.h"

#include <string.h>

#ifndef IPC_EG_MAX_GROUPS
#define IPC_EG_MAX_GROUPS 4
#endif

#define EG_LOCK_MS 1000u

typedef struct {
    ipc_sem_t sem;
    uint32_t  bits;
    bool      wait_all;
    bool      in_use;
    bool      satisfied;
    uint32_t  snapshot;   /* gia tri co tai luc thoa man */
} waiter_t;

struct ipc_event_group {
    ipc_mutex_t lock;
    volatile uint32_t bits;
    waiter_t waiters[IPC_EG_MAX_WAITERS];
    bool in_use;
};

static struct ipc_event_group s_groups[IPC_EG_MAX_GROUPS];

static bool condition_met(uint32_t current, uint32_t want, bool all)
{
    return all ? ((current & want) == want) : ((current & want) != 0);
}

ipc_event_group_t *ipc_event_group_create(void)
{
    ipc_event_group_t *g = NULL;
    ipc_enter_critical();
    for (int i = 0; i < IPC_EG_MAX_GROUPS; ++i) {
        if (!s_groups[i].in_use) { g = &s_groups[i]; g->in_use = true; break; }
    }
    ipc_exit_critical();
    if (!g) return NULL;

    g->bits = 0;
    memset(g->waiters, 0, sizeof(g->waiters));
    if (!g->lock) g->lock = ipc_mutex_create();
    if (!g->lock) {
        ipc_enter_critical();
        g->in_use = false;
        ipc_exit_critical();
        return NULL;
    }
    return g;
}

void ipc_event_group_destroy(ipc_event_group_t *g)
{
    if (!g) return;
    /* Danh thuc moi nguoi con cho de khong ai ket lai vinh vien. */
    if (ipc_mutex_lock(g->lock, EG_LOCK_MS)) {
        for (int i = 0; i < IPC_EG_MAX_WAITERS; ++i) {
            if (g->waiters[i].in_use && g->waiters[i].sem)
                ipc_sem_give(g->waiters[i].sem);
        }
        ipc_mutex_unlock(g->lock);
    }
    ipc_enter_critical();
    g->in_use = false;
    ipc_exit_critical();
}

uint32_t ipc_event_group_get(const ipc_event_group_t *g)
{
    return g ? g->bits : 0;
}

/* Phai giu lock. Danh thuc nhung nguoi cho da du dieu kien. */
static void wake_waiters(ipc_event_group_t *g)
{
    for (int i = 0; i < IPC_EG_MAX_WAITERS; ++i) {
        waiter_t *w = &g->waiters[i];
        if (!w->in_use || w->satisfied) continue;
        if (condition_met(g->bits, w->bits, w->wait_all)) {
            w->satisfied = true;
            w->snapshot = g->bits;
            ipc_sem_give(w->sem);
        }
    }
}

uint32_t ipc_event_group_set(ipc_event_group_t *g, uint32_t bits)
{
    if (!g) return 0;
    if (!ipc_mutex_lock(g->lock, EG_LOCK_MS)) return g->bits;
    g->bits |= bits;
    wake_waiters(g);
    uint32_t out = g->bits;
    ipc_mutex_unlock(g->lock);
    return out;
}

uint32_t ipc_event_group_clear(ipc_event_group_t *g, uint32_t bits)
{
    if (!g) return 0;
    if (!ipc_mutex_lock(g->lock, EG_LOCK_MS)) return g->bits;
    g->bits &= ~bits;
    uint32_t out = g->bits;
    ipc_mutex_unlock(g->lock);
    return out;
}

void ipc_event_group_set_from_isr(ipc_event_group_t *g, uint32_t bits,
                                  bool *higher_prio_woken)
{
    if (!g) return;
    /* Tu ISR khong lay mutex duoc: dat co trong critical section ngan, roi
     * danh thuc nguoi cho bang sem (an toan tu ISR). */
    ipc_enter_critical();
    g->bits |= bits;
    uint32_t now = g->bits;
    ipc_exit_critical();

    for (int i = 0; i < IPC_EG_MAX_WAITERS; ++i) {
        waiter_t *w = &g->waiters[i];
        if (!w->in_use || w->satisfied) continue;
        if (condition_met(now, w->bits, w->wait_all)) {
            w->satisfied = true;
            w->snapshot = now;
            ipc_sem_give_from_isr(w->sem, higher_prio_woken);
        }
    }
}

uint32_t ipc_event_group_wait(ipc_event_group_t *g, uint32_t bits, bool wait_all,
                              bool clear_on_exit, uint32_t timeout_ms)
{
    if (!g || bits == 0) return 0;
    if (!ipc_mutex_lock(g->lock, EG_LOCK_MS)) return 0;

    /* Duong nhanh: dieu kien da du roi thi khong dung den semaphore. */
    if (condition_met(g->bits, bits, wait_all)) {
        uint32_t out = g->bits;
        if (clear_on_exit) g->bits &= ~bits;
        ipc_mutex_unlock(g->lock);
        return out;
    }
    if (timeout_ms == 0) {   /* chi hoi tham, khong cho */
        ipc_mutex_unlock(g->lock);
        return 0;
    }

    waiter_t *w = NULL;
    for (int i = 0; i < IPC_EG_MAX_WAITERS; ++i) {
        if (!g->waiters[i].in_use) { w = &g->waiters[i]; break; }
    }
    if (!w) { ipc_mutex_unlock(g->lock); return 0; }   /* het cho cho -> khong chan vo han */

    if (!w->sem) w->sem = ipc_sem_create();
    if (!w->sem) { ipc_mutex_unlock(g->lock); return 0; }
    w->bits = bits;
    w->wait_all = wait_all;
    w->satisfied = false;
    w->snapshot = 0;
    w->in_use = true;
    ipc_mutex_unlock(g->lock);

    bool got = ipc_sem_take(w->sem, timeout_ms);

    if (!ipc_mutex_lock(g->lock, EG_LOCK_MS)) { w->in_use = false; return 0; }
    uint32_t out = 0;
    if (got && w->satisfied) {
        out = w->snapshot;
        if (clear_on_exit) g->bits &= ~bits;
    } else if (condition_met(g->bits, bits, wait_all)) {
        /* Het gio dung luc dieu kien vua du: khong bao that bai oan. */
        out = g->bits;
        if (clear_on_exit) g->bits &= ~bits;
    }
    w->in_use = false;
    w->satisfied = false;
    ipc_mutex_unlock(g->lock);
    return out;
}
