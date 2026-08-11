/*
 * app.c - lap rap he thong.
 *
 * Thu tu khoi dong co y nghia:
 *   config    truoc tien - moi nguoi khac doc cau hinh cua no
 *   health    thu hai    - de bat duoc su co ngay tu luc khoi dong
 *   processor -> uploader - nguoi tieu thu phai san sang truoc
 *   sensor    cuoi cung  - vi no la nguoi bat dau sinh du lieu
 *
 * Neu dao nguoc, nhung mau dau tien se roi vao khoang trong: bus khong co
 * nguoi nghe thi su kien bien mat khong dau vet.
 */
#include "services.h"

#include "ipc_event.h"
#include "ipc_event_group.h"
#include "ipc_service.h"
#include "ipc_health.h"
#include "ipc_supervisor.h"
#include "ipc_timer.h"
#include "ipc_watchdog.h"

#include <string.h>

static ipc_event_group_t *s_bits;
static app_cfg_t          s_cfg;
static bool               s_running;

ipc_event_group_t *app_bits(void) { return s_bits; }

void app_cfg_default(app_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->spawn_tasks = true;
}

ipc_looper_t *app_looper(const char *service_name)
{
    ipc_handler_t *h = ipc_service_get(service_name);
    if (h) return h->looper;

    /* Dich vu co the dang chet nen khong con trong ServiceManager. Tim theo
     * ten looper - ten looper va ten dich vu la mot. */
    uint32_t n = ipc_looper_count();
    for (uint32_t i = 0; i < n; ++i) {
        ipc_looper_t *lp = ipc_looper_at(i);
        if (lp && strcmp(ipc_looper_name(lp), service_name) == 0) return lp;
    }
    return NULL;
}

static bool watchdog_recover(const char *client, ipc_looper_t *lp, void *user)
{
    (void)client; (void)user;
    return lp ? ipc_supervisor_force_restart(lp) : false;
}

bool app_start(const app_cfg_t *cfg)
{
    if (s_running) return false;
    if (cfg) s_cfg = *cfg;
    else app_cfg_default(&s_cfg);

    ipc_message_pool_init();
    ipc_service_manager_init();
    ipc_bus_init();

    s_bits = ipc_event_group_create();
    if (!s_bits) return false;

    /* Timer engine phai chay truoc sensor, vi sensor dat nhip lay mau ngay
     * trong on_start. */
    ipc_timer_engine_cfg_t tc;
    ipc_timer_engine_cfg_default(&tc);
    tc.priority = 15;
    tc.own_task = s_cfg.spawn_tasks;
    ipc_timer_engine_start(&tc);

    if (!svc_config_start(&s_cfg))    return false;
    if (!svc_health_start(&s_cfg))    return false;
    if (!svc_processor_start(&s_cfg)) return false;
    if (!svc_uploader_start(&s_cfg))  return false;
    if (!svc_sensor_start(&s_cfg))    return false;

    if (s_cfg.spawn_tasks) {
        ipc_supervisor_cfg_t sc;
        ipc_supervisor_cfg_default(&sc);
        sc.priority = 20;
        sc.scan_interval_ms = 250;
        ipc_supervisor_start(&sc);

        ipc_wdt_cfg_t wc;
        ipc_wdt_cfg_default(&wc);
        wc.priority = 22;
        wc.recovery = watchdog_recover;
        wc.max_recovery_attempts = 3;
        ipc_wdt_start(&wc);

        ipc_wdt_watch_looper(app_looper(SVC_SENSOR), 0, IPC_WDT_POLICY_RESTART);
        ipc_wdt_watch_looper(app_looper(SVC_PROCESSOR), 0, IPC_WDT_POLICY_RESTART);
        ipc_wdt_watch_looper(app_looper(SVC_UPLOADER), 0, IPC_WDT_POLICY_RESTART);
        ipc_wdt_watch_looper(app_looper(SVC_CONFIG), 0, IPC_WDT_POLICY_LOG);

        /* Cho tat ca dich vu bao san sang. Neu qua 5 giay chua du thi co
         * dich vu khong len duoc - bao that bai chu khong chay nua voi. */
        uint32_t got = ipc_event_group_wait(s_bits, BIT_ALL_SERVICES, true,
                                            false, 5000);
        if (got == 0) return false;
    }

    s_running = true;
    return true;
}

void app_stop(void)
{
    ipc_supervisor_stop();
    ipc_wdt_stop();
    ipc_health_stop();
    ipc_timer_engine_stop();
    s_running = false;
}

uint32_t app_poll_all(void)
{
    uint32_t total = 0;
    /*
     * Mot su kien de ra su kien khac (sensor -> processor -> uploader), nen
     * phai lap den khi he thong lang han. Chan tren de mot vong lap su kien
     * vo tan khong treo bo test.
     */
    for (int round = 0; round < 32; ++round) {
        uint32_t this_round = 0;
        uint32_t n = ipc_looper_count();
        for (uint32_t i = 0; i < n; ++i) {
            ipc_looper_t *lp = ipc_looper_at(i);
            if (!lp) continue;
            if (ipc_looper_state(lp) == IPC_LOOPER_STOPPED) continue;
            this_round += ipc_looper_poll(lp, 0);
        }
        total += this_round;
        if (this_round == 0) break;
    }
    return total;
}
