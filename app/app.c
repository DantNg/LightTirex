/*
 * app.c - lap rap he thong.
 *
 * Toan bo danh sach dich vu nam trong MOT bang duy nhat ben duoi. Muon them
 * dich vu: them mot dong. Muon bo: xoa mot dong. Khong co doan code khoi
 * dong rieng cho tung dich vu nao ca.
 */
#include "app.h"
#include "common/services.h"

#include "ipc_event.h"
#include "ipc_event_group.h"
#include "ipc_health.h"
#include "ipc_service.h"
#include "ipc_supervisor.h"
#include "ipc_timer.h"
#include "ipc_watchdog.h"

#include <stdio.h>
#include <string.h>

/*
 * THU TU O DAY LA CO Y - do la thu tu phu thuoc, khong phai ngau nhien:
 *
 *   config    truoc tien  - moi nguoi khac doc cau hinh cua no
 *   health    thu hai     - de bat duoc su co ngay tu luc khoi dong
 *   processor -> uploader - nguoi tieu thu phai san sang truoc
 *   sensor    cuoi cung   - vi no la nguoi bat dau sinh du lieu
 *
 * Neu dao nguoc, nhung mau dau tien se roi vao khoang trong: bus khong co
 * nguoi nghe thi su kien bien mat khong dau vet.
 */
typedef app_service_t *(*service_factory_fn)(void);

static service_factory_fn const k_services[] = {
    configService,
    healthService,
    processorService,
    uploaderService,
    sensorService,
};

#define SERVICE_COUNT (sizeof(k_services) / sizeof(k_services[0]))

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
    app_service_t *svc = app_service_find(service_name);
    if (svc && svc->looper) return svc->looper;

    /* Du phong: tim theo ten looper (ten looper trung ten dich vu). */
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

/* Cac dich vu nen (khong phai app_service_t): giam sat va hoi phuc. */
static void start_supervision(void)
{
    ipc_supervisor_cfg_t sc;
    ipc_supervisor_cfg_default(&sc);
    sc.priority = 20;                  /* cao hon moi looper dich vu */
    sc.scan_interval_ms = 250;
    ipc_supervisor_start(&sc);

    ipc_wdt_cfg_t wc;
    ipc_wdt_cfg_default(&wc);
    wc.priority = 22;                  /* cao hon ca supervisor */
    wc.recovery = watchdog_recover;
    wc.max_recovery_attempts = 3;
    ipc_wdt_start(&wc);

    for (size_t i = 0; i < SERVICE_COUNT; ++i) {
        app_service_t *svc = k_services[i]();
        ipc_wdt_watch_looper(svc->looper, 0, IPC_WDT_POLICY_RESTART);
    }
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

    /* Timer engine phai chay truoc sensor: sensor dat nhip lay mau ngay
     * trong on_create. */
    ipc_timer_engine_cfg_t tc;
    ipc_timer_engine_cfg_default(&tc);
    tc.priority = 15;
    tc.own_task = s_cfg.spawn_tasks;
    ipc_timer_engine_start(&tc);

    /* Khoi dong tat ca dich vu qua cung mot duong. */
    for (size_t i = 0; i < SERVICE_COUNT; ++i) {
        app_service_t *svc = k_services[i]();
        if (!app_service_start(svc, &s_cfg, s_cfg.spawn_tasks)) return false;
    }

    if (s_cfg.spawn_tasks) {
        start_supervision();

        /* Cho tat ca dich vu bao san sang. Qua 5 giay chua du nghia la co
         * dich vu khong len duoc - bao that bai chu khong chay nua voi. */
        if (ipc_event_group_wait(s_bits, BIT_ALL_SERVICES, true, false, 5000) == 0)
            return false;
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
            if (!lp || ipc_looper_state(lp) == IPC_LOOPER_STOPPED) continue;
            this_round += ipc_looper_poll(lp, 0);
        }
        total += this_round;
        if (this_round == 0) break;
    }
    return total;
}

void app_dump(void (*print)(const char *line))
{
    if (!print) return;
    char line[160];

    for (size_t i = 0; i < SERVICE_COUNT; ++i) {
        app_service_t *svc = k_services[i]();
        snprintf(line, sizeof(line),
                 "svc %-10s state=%d gen=%u restarts=%u pending=%u "
                 "handled=%u unhandled=%u",
                 svc->name, (int)ipc_looper_state(svc->looper),
                 (unsigned)ipc_looper_generation(svc->looper),
                 (unsigned)ipc_looper_restart_count(svc->looper),
                 (unsigned)ipc_looper_pending(svc->looper),
                 (unsigned)svc->handled, (unsigned)svc->unhandled);
        print(line);
    }
    snprintf(line, sizeof(line), "bits=0x%02x",
             (unsigned)ipc_event_group_get(s_bits));
    print(line);
    ipc_bus_dump(print);
    ipc_health_dump(print);
}
