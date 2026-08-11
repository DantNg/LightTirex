#include "ipc_watchdog.h"
#include "ipc_clock.h"

#include <stdio.h>
#include <string.h>

#define WDT_LOCK_MS 1000u

struct ipc_wdt_client {
    char name[IPC_WDT_NAME_LEN];
    uint32_t timeout_ms;
    ipc_wdt_policy_t policy;

    ipc_looper_t *looper;       /* != NULL: doc heartbeat thay vi cho kick */
    volatile ipc_tick_t last_kick;

    bool in_use;
    bool suspended;
    bool expired;               /* dang trong trang thai qua han */
    uint32_t recovery_attempts; /* so lan phuc hoi that bai lien tiep */
};

static struct ipc_wdt_client s_clients[IPC_MAX_WDT_CLIENTS];
static ipc_wdt_cfg_t   s_cfg;
static ipc_wdt_backend_t *s_be;
static ipc_mutex_t     s_lock;
static ipc_task_t      s_task;
static volatile bool   s_running;
static ipc_wdt_stats_t s_stats;

/* ------------------------------------------------------------------ */
/* Backend                                                             */
/* ------------------------------------------------------------------ */

static bool noop_init(ipc_wdt_backend_t *self, uint32_t t) { (void)self; (void)t; return true; }
static void noop_feed(ipc_wdt_backend_t *self) { (void)self; }
static void noop_reset(ipc_wdt_backend_t *self) { (void)self; }

static ipc_wdt_backend_t s_noop = {
    .name = "noop",
    .init = noop_init,
    .feed = noop_feed,
    .reset_system = noop_reset,
    .impl = 0,
};

ipc_wdt_backend_t *ipc_wdt_backend_noop(void) { return &s_noop; }

static bool fake_init(ipc_wdt_backend_t *self, uint32_t t)
{
    ipc_wdt_fake_backend_t *fb = (ipc_wdt_fake_backend_t *)self->impl;
    fb->init_count++;
    fb->last_timeout_ms = t;
    return true;
}
static void fake_feed(ipc_wdt_backend_t *self)
{
    ((ipc_wdt_fake_backend_t *)self->impl)->feed_count++;
}
static void fake_reset(ipc_wdt_backend_t *self)
{
    /* Tren desktop khong reset that - chi ghi nhan de test kiem chung. */
    ((ipc_wdt_fake_backend_t *)self->impl)->reset_count++;
}

void ipc_wdt_fake_backend_init(ipc_wdt_fake_backend_t *fb)
{
    memset(fb, 0, sizeof(*fb));
    fb->base.name = "fake";
    fb->base.init = fake_init;
    fb->base.feed = fake_feed;
    fb->base.reset_system = fake_reset;
    fb->base.impl = fb;
}

/* ------------------------------------------------------------------ */
/* Core                                                                */
/* ------------------------------------------------------------------ */

void ipc_wdt_cfg_default(ipc_wdt_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->backend = ipc_wdt_backend_noop();
    cfg->hw_timeout_ms = 0;
    cfg->check_interval_ms = 500;
    cfg->priority = 22;
    cfg->stack_words = 3072;
    cfg->own_task = true;
    cfg->max_recovery_attempts = 3;
}

static bool ensure_init(void)
{
    if (!s_lock) s_lock = ipc_mutex_create();
    return s_lock != NULL;
}

static ipc_wdt_handle_t alloc_client(const char *name, uint32_t timeout_ms,
                                     ipc_wdt_policy_t policy, ipc_looper_t *lp)
{
    if (!ensure_init()) return NULL;
    if (timeout_ms == 0) return NULL;
    if (!ipc_mutex_lock(s_lock, WDT_LOCK_MS)) return NULL;

    ipc_wdt_client_t *c = NULL;
    for (int i = 0; i < IPC_MAX_WDT_CLIENTS; ++i) {
        if (!s_clients[i].in_use) { c = &s_clients[i]; break; }
    }
    if (c) {
        memset(c, 0, sizeof(*c));
        snprintf(c->name, sizeof(c->name), "%s", name ? name : "wdt");
        c->timeout_ms = timeout_ms;
        c->policy = policy;
        c->looper = lp;
        c->last_kick = ipc_clock_now();
        c->in_use = true;
        s_stats.clients++;
    }
    ipc_mutex_unlock(s_lock);
    return c;
}

ipc_wdt_handle_t ipc_wdt_register(const char *name, uint32_t timeout_ms,
                                  ipc_wdt_policy_t policy)
{
    return alloc_client(name, timeout_ms, policy, NULL);
}

ipc_wdt_handle_t ipc_wdt_watch_looper(ipc_looper_t *lp, uint32_t timeout_ms,
                                      ipc_wdt_policy_t policy)
{
    if (!lp) return NULL;
    if (timeout_ms == 0) {
        const ipc_looper_cfg_t *lc = ipc_looper_cfg(lp);
        timeout_ms = lc ? lc->heartbeat_timeout_ms : 0;
        if (timeout_ms == 0) return NULL;   /* looper tat giam sat -> khong ep */
        timeout_ms *= 2;  /* rong hon nguong cua supervisor: watchdog la luoi cuoi */
    }
    return alloc_client(ipc_looper_name(lp), timeout_ms, policy, lp);
}

void ipc_wdt_unregister(ipc_wdt_handle_t h)
{
    if (!h || !s_lock) return;
    if (!ipc_mutex_lock(s_lock, WDT_LOCK_MS)) return;
    if (h->in_use) { h->in_use = false; if (s_stats.clients) s_stats.clients--; }
    ipc_mutex_unlock(s_lock);
}

/* Kick phai that re: goi tu vong lap nong, tu ISR. Khong lay mutex. */
void ipc_wdt_kick(ipc_wdt_handle_t h)
{
    if (!h) return;
    h->last_kick = ipc_clock_now();
    h->expired = false;
    h->recovery_attempts = 0;
}

void ipc_wdt_suspend(ipc_wdt_handle_t h) { if (h) h->suspended = true; }

void ipc_wdt_resume(ipc_wdt_handle_t h)
{
    if (!h) return;
    h->last_kick = ipc_clock_now();   /* cho lai tu dau, khong phat oan */
    h->suspended = false;
    h->expired = false;
}

static uint32_t overdue_of(const ipc_wdt_client_t *c, ipc_tick_t now)
{
    uint32_t idle = c->looper ? ipc_looper_since_heartbeat_ms(c->looper)
                              : (uint32_t)(now - c->last_kick);
    return idle > c->timeout_ms ? idle - c->timeout_ms : 0;
}

static void escalate(ipc_wdt_client_t *c, uint32_t overdue)
{
    ipc_wdt_policy_t action = c->policy;
    bool recovered = false;

    if (action == IPC_WDT_POLICY_RESTART) {
        if (s_cfg.recovery) {
            recovered = s_cfg.recovery(c->name, c->looper, s_cfg.user);
        }
        if (recovered) {
            s_stats.recoveries_ok++;
            c->recovery_attempts = 0;
            c->last_kick = ipc_clock_now();
            c->expired = false;
        } else {
            s_stats.recoveries_failed++;
            c->recovery_attempts++;
            /* Phuc hoi mai khong duoc -> leo thang len reset ca he thong. */
            if (s_cfg.max_recovery_attempts &&
                c->recovery_attempts >= s_cfg.max_recovery_attempts) {
                action = IPC_WDT_POLICY_RESET;
            }
        }
    }

    if (s_cfg.on_event) s_cfg.on_event(c->name, overdue, action, recovered, s_cfg.user);

    if (action == IPC_WDT_POLICY_RESET) {
        /* Khong tro ve tren phan cung that. */
        if (s_be && s_be->reset_system) s_be->reset_system(s_be);
    }
}

uint32_t ipc_wdt_check(void)
{
    if (!s_lock) return 0;
    ipc_tick_t now = ipc_clock_now();

    /* Chup danh sach client dang qua han roi xu ly ngoai vung khoa: escalate
     * co the goi supervisor (tao task, ngu backoff) - khong duoc giu lock. */
    ipc_wdt_client_t *bad[IPC_MAX_WDT_CLIENTS];
    uint32_t overdue[IPC_MAX_WDT_CLIENTS];
    uint32_t n = 0;

    if (!ipc_mutex_lock(s_lock, WDT_LOCK_MS)) return 0;
    for (int i = 0; i < IPC_MAX_WDT_CLIENTS; ++i) {
        ipc_wdt_client_t *c = &s_clients[i];
        if (!c->in_use || c->suspended) continue;
        /* Looper da dung han thi khong tinh la qua han. */
        if (c->looper && ipc_looper_state(c->looper) != IPC_LOOPER_RUNNING) continue;

        uint32_t od = overdue_of(c, now);
        if (od > 0) {
            if (!c->expired) { c->expired = true; s_stats.expiries++; }
            bad[n] = c;
            overdue[n] = od;
            n++;
        }
    }
    ipc_mutex_unlock(s_lock);

    for (uint32_t i = 0; i < n; ++i) escalate(bad[i], overdue[i]);

    /*
     * Chi cho HW watchdog an khi khong con ai qua han. Neu chinh task nay
     * chet/treo, ham nay khong chay -> HW WDT het gio -> chip reset.
     */
    if (n == 0 && s_be && s_be->feed && s_cfg.hw_timeout_ms) {
        s_be->feed(s_be);
        s_stats.feeds++;
    }
    return n;
}

static void wdt_task(void *arg)
{
    (void)arg;
    while (s_running) {
        ipc_wdt_check();
        ipc_sleep_ms(s_cfg.check_interval_ms);
    }
    s_task = NULL;
    ipc_task_delete(ipc_task_self());
}

bool ipc_wdt_start(const ipc_wdt_cfg_t *cfg)
{
    if (!ensure_init()) return false;
    if (cfg) s_cfg = *cfg;
    else ipc_wdt_cfg_default(&s_cfg);
    if (s_cfg.check_interval_ms == 0) s_cfg.check_interval_ms = 500;
    if (s_cfg.stack_words == 0) s_cfg.stack_words = 3072;

    s_be = s_cfg.backend ? s_cfg.backend : ipc_wdt_backend_noop();
    if (s_cfg.hw_timeout_ms && s_be->init) {
        if (!s_be->init(s_be, s_cfg.hw_timeout_ms)) return false;
    }

    if (!s_cfg.own_task) return true;   /* che do test */
    if (s_running) return true;

    s_running = true;
    if (!ipc_task_create(&s_task, "ipc_wdt", wdt_task, NULL,
                         s_cfg.stack_words, s_cfg.priority)) {
        s_running = false;
        return false;
    }
    return true;
}

void ipc_wdt_stop(void) { s_running = false; }

void ipc_wdt_get_stats(ipc_wdt_stats_t *out)
{
    if (!out) return;
    if (s_lock && ipc_mutex_lock(s_lock, WDT_LOCK_MS)) {
        *out = s_stats;
        ipc_mutex_unlock(s_lock);
    } else {
        *out = s_stats;
    }
}

void ipc_wdt_dump(void (*print)(const char *line))
{
    if (!print || !s_lock || !ipc_mutex_lock(s_lock, WDT_LOCK_MS)) return;
    char line[128];
    ipc_tick_t now = ipc_clock_now();
    for (int i = 0; i < IPC_MAX_WDT_CLIENTS; ++i) {
        ipc_wdt_client_t *c = &s_clients[i];
        if (!c->in_use) continue;
        uint32_t idle = c->looper ? ipc_looper_since_heartbeat_ms(c->looper)
                                  : (uint32_t)(now - c->last_kick);
        snprintf(line, sizeof(line),
                 "wdt[%2d] %-12s %s idle=%ums/%ums pol=%d %s",
                 i, c->name, c->looper ? "looper" : "manual",
                 (unsigned)idle, (unsigned)c->timeout_ms, (int)c->policy,
                 c->suspended ? "SUSPENDED" : (c->expired ? "EXPIRED" : "ok"));
        print(line);
    }
    ipc_mutex_unlock(s_lock);
}
