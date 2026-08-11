/*
 * svc_health.c - dat bang luat cho he thong nay va noi health voi supervisor.
 *
 * Core ipc_health khong biet gi ve cam bien hay server. No chi biet
 * "exception + luat -> hanh dong". File nay la noi khai bao CHINH SACH
 * rieng cua ung dung: loi nao thi nhin nhan, loi nao thi khoi dong lai,
 * loi nao thi reboot.
 *
 * Do la ranh gioi quan trong: doi chinh sach thi sua file nay, khong ai
 * phai dong vao ipc_health.c.
 */
#include "services.h"

#include "ipc_event.h"
#include "ipc_health.h"
#include "ipc_service.h"
#include "ipc_supervisor.h"

static ipc_looper_t *s_lp;
static ipc_handler_t s_h;
static uint32_t      s_alerts_seen;
static bool          s_degraded;

/* Health khong biet supervisor la gi - ta noi hai ben lai o day. */
static bool recover(const char *service, ipc_looper_t *lp, void *user)
{
    (void)user;
    if (!lp) lp = app_looper(service);
    if (!lp) return false;

    /*
     * Khong co task rieng nghia la ai do dang tu bom nhip cho looper nay
     * (che do test tren desktop). Luc do tao them mot task that se pha vo
     * tinh don luong, nen ta khoi dong lai tai cho - van chay dung duong
     * on_start ma task that se chay.
     */
    if (!ipc_looper_is_task_driven(lp)) return ipc_looper_restart_inplace(lp);
    return ipc_supervisor_force_restart(lp);
}

static void on_health_event(const char *source, uint32_t code, ipc_severity_t sev,
                            ipc_health_action_t act, int32_t detail, void *user)
{
    (void)code; (void)detail; (void)user; (void)source;

    /* Cong bo trang thai suc khoe kem co GIU LAI: dich vu nao vua hoi sinh
     * va dang ky nghe muon cung biet ngay he thong dang o tinh trang gi. */
    bool bad = (sev >= IPC_SEV_ERROR) || (act >= IPC_ACT_RESTART_SERVICE);
    if (bad != s_degraded) {
        s_degraded = bad;
        ipc_bus_publish_retained(TOPIC_HEALTH_STATE, bad ? 0 : 1, (int32_t)sev);
    }
}

static bool health_cb(ipc_handler_t *h, ipc_message_t *m, void *user)
{
    (void)h; (void)user;
    if (m->what == MSG_EV_ALERT) {
        /* Canh bao nguong khong phai loi he thong: dem lai de con nhin
         * xu huong, khong khoi dong lai ai ca. */
        s_alerts_seen++;
    }
    return true;
}

static void install_rules(void)
{
    ipc_health_clear_rules();

    const ipc_health_rule_t rules[] = {
        /* Doc cam bien hong le te la binh thuong (nhieu, I2C ban). Chi khi
         * hong 3 lan trong 10 giay moi khoi dong lai dich vu cam bien.
         * Nguong nam O DAY chu khong nam trong svc_sensor.c: doc mot cho la
         * biet duoc luc nao dich vu se bi khoi dong lai. */
        { EXC_SENSOR_READ, SVC_SENSOR, IPC_SEV_WARN, 3, 10000,
          IPC_ACT_RESTART_SERVICE },

        /* Day len that bai la chuyen thuong khi khong co mang - uploader da
         * co hang doi va co che thu lai roi, nen chi ghi nhan. */
        { EXC_UPLOAD_FAIL, SVC_UPLOADER, IPC_SEV_WARN, 1, 0, IPC_ACT_LOG },

        /* Mat du lieu do hang doi tran thi phai thay duoc, nhung khong dong
         * cham gi vao he thong dang chay. */
        { EXC_QUEUE_OVERFLOW, NULL, IPC_SEV_WARN, 1, 0, IPC_ACT_LOG },

        /* Sap het bo nho: khoi dong lai dich vu ngon nhat truoc khi ca he
         * thong chet. */
        { IPC_EXC_OOM, NULL, IPC_SEV_ERROR, 1, 0, IPC_ACT_RESTART_SERVICE },

        /* Loi chet nguoi thi reboot. Luat rong nhat nen dat CUOI cung. */
        { IPC_EXC_ANY, NULL, IPC_SEV_FATAL, 1, 0, IPC_ACT_REBOOT },
    };

    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); ++i)
        ipc_health_add_rule(&rules[i]);
}

static void health_on_start(ipc_looper_t *lp, void *user)
{
    const app_cfg_t *acfg = (const app_cfg_t *)user;

    ipc_bus_unsubscribe_service(SVC_HEALTH);

    ipc_handler_init(&s_h, lp, health_cb, NULL, SVC_HEALTH);
    ipc_service_register(SVC_HEALTH, &s_h);
    ipc_bus_subscribe_service(TOPIC_ALERT, SVC_HEALTH, MSG_EV_ALERT);

    ipc_health_cfg_t hc;
    ipc_health_cfg_default(&hc);
    hc.recovery = recover;
    hc.on_event = on_health_event;
    hc.queue_depth_warn = 48;
    hc.pool_free_warn = 6;
    hc.check_interval_ms = 500;
    hc.priority = 18;
    hc.own_task = acfg->spawn_tasks;   /* test tu goi ipc_health_check() */
    ipc_health_start(&hc);

    install_rules();
    ipc_bus_publish_retained(TOPIC_HEALTH_STATE, 1, 0);
    ipc_event_group_set(app_bits(), BIT_HEALTH_READY);
}

bool svc_health_start(const app_cfg_t *cfg)
{
    ipc_looper_cfg_t lc;
    ipc_looper_cfg_default(&lc, SVC_HEALTH);
    lc.priority = 8;
    lc.heartbeat_timeout_ms = 5000;
    lc.on_start = health_on_start;
    lc.user = (void *)cfg;

    s_lp = ipc_looper_create(&lc);
    if (!s_lp) return false;

    if (cfg->spawn_tasks) return ipc_looper_start(s_lp);
    health_on_start(s_lp, (void *)cfg);
    return true;
}
