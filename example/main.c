/*
 * Vi du: 3 dich vu chay tren 3 looper rieng + supervisor.
 *
 *   sensor  : dinh ky tu gui message cho chinh minh (giong postDelayed)
 *   worker  : nhan viec tu sensor, thinh thoang "chet" de thu nghiem
 *   ui      : dang ky death recipient de biet worker song/chet
 *
 * Build voi ESP-IDF: dat thu muc nay lam component hoac them vao main.
 */
#include "ipc_looper.h"
#include "ipc_service.h"
#include "ipc_supervisor.h"
#include "ipc_timer.h"
#include "ipc_watchdog.h"
#include "ipc_health.h"
#include "ipc_config.h"

#include <stdio.h>
#include <string.h>

enum {
    MSG_SENSOR_TICK = 1,
    MSG_WORK_ITEM,
    MSG_WORK_CRASH,      /* co tinh lam task chet */
    MSG_WORK_HANG,       /* co tinh treo -> heartbeat bat duoc */
    MSG_UI_NOTIFY,
};

static ipc_looper_t *g_sensor_lp, *g_worker_lp, *g_ui_lp;
static ipc_handler_t g_sensor_h, g_worker_h, g_ui_h;

/* ------------------------- sensor ------------------------- */

static bool sensor_cb(ipc_handler_t *h, ipc_message_t *m, void *user)
{
    (void)user;
    static int seq;
    if (m->what == MSG_SENSOR_TICK) {
        ipc_service_send("worker", MSG_WORK_ITEM, seq++, 0);
        /* Tu hen gio lai: y het Handler.sendEmptyMessageDelayed. */
        ipc_handler_send_empty_delayed(h, MSG_SENSOR_TICK, 1000);
    }
    return true;
}

static void sensor_on_start(ipc_looper_t *lp, void *user)
{
    (void)lp; (void)user;
    /* Chay TRONG task moi -> an toan khi khoi tao lai sau crash. */
    ipc_handler_init(&g_sensor_h, g_sensor_lp, sensor_cb, NULL, "sensor");
    ipc_service_register("sensor", &g_sensor_h);
    ipc_handler_send_empty(&g_sensor_h, MSG_SENSOR_TICK);
}

/* ------------------------- worker ------------------------- */

static bool worker_cb(ipc_handler_t *h, ipc_message_t *m, void *user)
{
    (void)h; (void)user;
    switch (m->what) {
    case MSG_WORK_ITEM:
        printf("[worker] item %d (gen=%u)\n", (int)m->arg1,
               (unsigned)ipc_looper_generation(g_worker_lp));
        if (m->arg1 == 5) {
            /* Mo phong task chet dot ngot: khong don dep gi ca. */
            printf("[worker] CHET DOT NGOT\n");
            ipc_task_delete(ipc_task_self());
        }
        break;
    case MSG_WORK_HANG:
        printf("[worker] treo vinh vien\n");
        for (;;) { /* khong yield -> heartbeat dung -> supervisor giet + tao lai */ }
    default:
        break;
    }
    return true;
}

static void worker_on_start(ipc_looper_t *lp, void *user)
{
    (void)user;
    ipc_handler_init(&g_worker_h, lp, worker_cb, NULL, "worker");
    ipc_service_register("worker", &g_worker_h);
    printf("[worker] khoi dong, generation=%u, con %u message cho\n",
           (unsigned)ipc_looper_generation(lp), (unsigned)ipc_looper_pending(lp));
}

static void worker_on_death(ipc_looper_t *lp, ipc_death_reason_t why,
                            uint32_t gen, void *user)
{
    (void)lp; (void)user;
    printf("[worker] phat hien chet: reason=%d gen=%u\n", (int)why, (unsigned)gen);
}

/* --------------------------- ui --------------------------- */

static bool ui_cb(ipc_handler_t *h, ipc_message_t *m, void *user)
{
    (void)h; (void)user;
    if (m->what == MSG_UI_NOTIFY)
        printf("[ui] worker %s (gen=%d)\n", m->arg1 ? "SONG" : "CHET", (int)m->arg2);
    return true;
}

/* Chay tren task supervisor -> chi day message, khong lam vic nang. */
static void on_worker_state(const char *svc, bool alive, uint32_t gen, void *user)
{
    (void)svc; (void)user;
    ipc_message_t *m = ipc_message_obtain();
    if (!m) return;
    m->what = MSG_UI_NOTIFY;
    m->arg1 = alive ? 1 : 0;
    m->arg2 = (int32_t)gen;
    ipc_handler_send(&g_ui_h, m);
}

static void ui_on_start(ipc_looper_t *lp, void *user)
{
    (void)user;
    ipc_handler_init(&g_ui_h, lp, ui_cb, NULL, "ui");
    ipc_service_register("ui", &g_ui_h);
    ipc_service_link_to_death("worker", on_worker_state, NULL);
}

/* ------------------------- bootstrap ------------------------- */

static void sup_log(const char *svc, ipc_death_reason_t why, uint32_t n, bool ok)
{
    printf("[super] %s chet (reason=%d), restart#%u -> %s\n",
           svc, (int)why, (unsigned)n, ok ? "OK" : "THAT BAI");
}

/*
 * Watchdog khong biet supervisor la gi - ta TIEM cach phuc hoi vao. Nho vay
 * doi supervisor sang co che khac (vd reset tung driver) khong phai sua
 * mot dong nao trong ipc_watchdog.c.
 */
static bool demo_recover(const char *client, ipc_looper_t *lp, void *user)
{
    (void)client; (void)user;
    return lp ? ipc_supervisor_force_restart(lp) : false;
}

static void wdt_log(const char *client, uint32_t overdue, ipc_wdt_policy_t act,
                    bool recovered, void *user)
{
    (void)user;
    printf("[wdt] %s tre %ums, action=%d, phuc hoi=%s\n",
           client, (unsigned)overdue, (int)act, recovered ? "co" : "khong");
}

static void health_event(const char *source, uint32_t code, ipc_severity_t sev,
                         ipc_health_action_t act, int32_t detail, void *user)
{
    (void)user;
    printf("[health] %s code=%u sev=%d detail=%d -> action=%d\n",
           source, (unsigned)code, (int)sev, (int)detail, (int)act);
}

/* Cau hinh: khai bao khoa + mac dinh. File hong hay thieu khoa thi ve mac
 * dinh chu khong chay voi rac. */
static const ipc_cfg_schema_t g_schema[] = {
    { "sensor.hz",   IPC_CFG_INT,  10, NULL      },
    { "log.enabled", IPC_CFG_BOOL, 1,  NULL      },
    { "device.name", IPC_CFG_STR,  0,  "tirex-1" },
};

static void cfg_changed(const char *key, void *user)
{
    (void)user;
    printf("[cfg] %s doi gia tri\n", key);
    /* Ap dung nong: doi tan so lay mau khong can khoi dong lai. */
    if (strcmp(key, "sensor.hz") == 0)
        ipc_service_send("sensor", MSG_SENSOR_TICK, ipc_cfg_get_int(key, 10), 0);
}

void ipc_demo_start(void)
{
    ipc_message_pool_init();
    ipc_service_manager_init();

    ipc_looper_cfg_t cfg;

    ipc_looper_cfg_default(&cfg, "sensor");
    cfg.priority = 5;
    cfg.on_start = sensor_on_start;
    g_sensor_lp = ipc_looper_create(&cfg);

    ipc_looper_cfg_default(&cfg, "worker");
    cfg.priority = 6;
    cfg.heartbeat_timeout_ms = 3000;
    cfg.max_restarts = 10;
    cfg.purge_queue_on_restart = false; /* giu lai viec dang cho */
    cfg.on_start = worker_on_start;
    cfg.on_death = worker_on_death;
    g_worker_lp = ipc_looper_create(&cfg);

    ipc_looper_cfg_default(&cfg, "ui");
    cfg.priority = 4;
    cfg.on_start = ui_on_start;
    g_ui_lp = ipc_looper_create(&cfg);

    ipc_looper_start(g_ui_lp);
    ipc_looper_start(g_worker_lp);
    ipc_looper_start(g_sensor_lp);

    ipc_supervisor_cfg_t scfg;
    ipc_supervisor_cfg_default(&scfg);
    scfg.priority = 20;      /* cao hon tat ca looper */
    scfg.scan_interval_ms = 250;
    scfg.on_event = sup_log;
    ipc_supervisor_start(&scfg);

    /* Timer engine: nam giua looper nghiep vu va supervisor ve do uu tien. */
    ipc_timer_engine_cfg_t tcfg;
    ipc_timer_engine_cfg_default(&tcfg);
    tcfg.priority = 15;
    ipc_timer_engine_start(&tcfg);

    /* Ban dinh ky THEO TEN: worker chet roi hoi sinh van nhan duoc tiep. */
    ipc_timer_send_periodic_to("worker", MSG_WORK_ITEM, 2000);

    /* Watchdog: luoi an toan cuoi cung, rong hon nguong cua supervisor. */
    ipc_wdt_cfg_t wcfg;
    ipc_wdt_cfg_default(&wcfg);
#ifdef ESP_PLATFORM
    wcfg.backend = ipc_wdt_backend_esp32();
    wcfg.hw_timeout_ms = 10000;
#endif
    wcfg.priority = 22;              /* cao hon ca supervisor */
    wcfg.recovery = demo_recover;
    wcfg.on_event = wdt_log;
    wcfg.max_recovery_attempts = 3;  /* phuc hoi 3 lan khong duoc -> reset chip */
    ipc_wdt_start(&wcfg);

    ipc_wdt_watch_looper(g_worker_lp, 0, IPC_WDT_POLICY_RESTART);
    ipc_wdt_watch_looper(g_sensor_lp, 0, IPC_WDT_POLICY_RESTART);
    ipc_wdt_watch_looper(g_ui_lp, 0, IPC_WDT_POLICY_LOG);

    /* --- config: nap tu file, ghi lai gop nhieu lan set --- */
    ipc_cfg_cfg_t ccfg;
    ipc_cfg_cfg_default(&ccfg);
    ccfg.storage = ipc_cfg_storage_file("/spiffs/app.cfg");
    ccfg.schema = g_schema;
    ccfg.schema_count = sizeof(g_schema) / sizeof(g_schema[0]);
    ccfg.on_change = cfg_changed;
    ccfg.autosave_delay_ms = 1000;   /* gop lai, do mon flash */
    ccfg.writer_looper = g_worker_lp; /* ghi file tren looper, khong nghen timer */
    if (ipc_cfg_init(&ccfg) == IPC_CFG_ERR_CORRUPT)
        ipc_health_report("config", IPC_EXC_PROTOCOL, IPC_SEV_WARN, 0);

    /* --- health: bang luat quyet dinh lam gi khi co su co --- */
    ipc_health_cfg_t hcfg;
    ipc_health_cfg_default(&hcfg);
    hcfg.backend = wcfg.backend;      /* dung chung duong reset voi watchdog */
    hcfg.recovery = demo_recover;
    hcfg.on_event = health_event;
    hcfg.heap_warn_bytes = 40000;
    hcfg.heap_critical_bytes = 12000;
    hcfg.queue_depth_warn = 32;
    hcfg.priority = 18;
    ipc_health_start(&hcfg);

    /* Thu tu dang ky = thu tu uu tien: hep truoc, rong sau. */
    ipc_health_rule_t rules[] = {
        /* Loi I2C le te thi bo qua; 5 lan trong 10 giay moi la hong that. */
        { IPC_EXC_HW_FAULT, "i2c", IPC_SEV_WARN, 5, 10000, IPC_ACT_RESTART_SERVICE },
        /* Dich vu noi giao thuc lung tung 3 lan -> giet han, khong hoi sinh. */
        { IPC_EXC_PROTOCOL, "modem", IPC_SEV_ERROR, 3, 30000, IPC_ACT_KILL_SERVICE },
        /* Sap het heap -> ha ve che do toi thieu truoc khi qua muon. */
        { IPC_EXC_LOW_HEAP, NULL, IPC_SEV_ERROR, 1, 0, IPC_ACT_SAFE_MODE },
        /* Bat ky loi chet nguoi nao -> reboot. */
        { IPC_EXC_ANY, NULL, IPC_SEV_FATAL, 1, 0, IPC_ACT_REBOOT },
    };
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); ++i)
        ipc_health_add_rule(&rules[i]);
}

#ifdef ESP_PLATFORM
void app_main(void) { ipc_demo_start(); }
#endif
