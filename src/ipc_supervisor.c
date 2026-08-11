#include "ipc_supervisor.h"
#include "ipc_service.h"

#include <string.h>

static ipc_supervisor_cfg_t s_cfg;
static ipc_task_t s_task;
static volatile bool s_running;

void ipc_supervisor_cfg_default(ipc_supervisor_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->scan_interval_ms = 500;
    cfg->priority = 20;      /* cao hon cac looper thuong */
    cfg->stack_words = 3072;
}

static void handle_death(ipc_looper_t *lp, ipc_death_reason_t why)
{
    const ipc_looper_cfg_t *cfg = ipc_looper_cfg(lp);

    /* 1. Bao cho client biet dich vu tam thoi bien mat (linkToDeath). */
    ipc_service_notify_state(lp, false);
    if (cfg && cfg->on_death)
        cfg->on_death(lp, why, ipc_looper_generation(lp), cfg->user);

    /* 2. Hoi sinh: task moi, van dung object looper cu -> hang doi con nguyen. */
    bool ok = ipc_looper_revive(lp, why);

    if (s_cfg.on_event)
        s_cfg.on_event(ipc_looper_name(lp), why, ipc_looper_restart_count(lp), ok);

    /* 3. Chi mo lai dich vu khi task moi thuc su chay. */
    if (ok) ipc_service_notify_state(lp, true);
}

void ipc_supervisor_scan_once(void)
{
    uint32_t n = ipc_looper_count();
    for (uint32_t i = 0; i < n; ++i) {
        ipc_looper_t *lp = ipc_looper_at(i);
        if (!lp) continue;

        ipc_death_reason_t why;
        if (ipc_looper_check_alive(lp, &why)) continue;
        handle_death(lp, why);
    }
}

static void supervisor_task(void *arg)
{
    (void)arg;
    while (s_running) {
        ipc_supervisor_scan_once();
        ipc_sleep_ms(s_cfg.scan_interval_ms);
    }
    s_task = NULL;
    ipc_task_delete(ipc_task_self());
}

bool ipc_supervisor_start(const ipc_supervisor_cfg_t *cfg)
{
    if (s_running) return false;
    if (cfg) s_cfg = *cfg;
    else ipc_supervisor_cfg_default(&s_cfg);
    if (s_cfg.scan_interval_ms == 0) s_cfg.scan_interval_ms = 500;
    if (s_cfg.stack_words == 0) s_cfg.stack_words = 3072;

    s_running = true;
    if (!ipc_task_create(&s_task, "ipc_super", supervisor_task, NULL,
                         s_cfg.stack_words, s_cfg.priority)) {
        s_running = false;
        return false;
    }
    return true;
}

void ipc_supervisor_stop(void) { s_running = false; }

bool ipc_supervisor_force_restart(ipc_looper_t *lp)
{
    if (!lp) return false;
    handle_death(lp, IPC_DEATH_MANUAL);
    return ipc_looper_state(lp) == IPC_LOOPER_RUNNING;
}
