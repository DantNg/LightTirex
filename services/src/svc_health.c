/*
 * svc_health.c - bang luat rieng cua ung dung + noi health voi supervisor.
 *
 * Core ipc_health khong biet gi ve cam bien hay server. No chi biet
 * "exception + luat -> hanh dong". File nay la noi khai bao CHINH SACH:
 * loi nao thi ghi nhan, loi nao thi khoi dong lai, loi nao thi reboot.
 *
 * Do la ranh gioi quan trong: doi chinh sach thi sua file nay, khong ai
 * phai dong vao core.
 */
#include "app.h"
#include "services.h"

#include "ipc_event.h"
#include "ipc_health.h"
#include "ipc_supervisor.h"

typedef struct {
    uint32_t alerts_seen;
    bool     degraded;
} health_state_t;

static health_state_t s_state;

/* Health khong biet supervisor la gi - ta noi hai ben lai o day. */
static bool recover(const char *service, ipc_looper_t *lp, void *user)
{
    (void)user;
    if (!lp) lp = app_looper(service);
    if (!lp) return false;

    /*
     * Khong co task rieng nghia la ai do dang tu bom nhip cho looper nay
     * (che do test tren desktop). Luc do tao them mot task that se pha vo
     * tinh don luong, nen khoi dong lai tai cho - van chay dung duong
     * on_create/on_subscribe ma task that se chay.
     */
    if (!ipc_looper_is_task_driven(lp)) return ipc_looper_restart_inplace(lp);
    return ipc_supervisor_force_restart(lp);
}

static void on_health_event(const char *source, uint32_t code, ipc_severity_t sev,
                            ipc_health_action_t act, int32_t detail, void *user)
{
    (void)source; (void)code; (void)detail; (void)user;

    /* Cong bo trang thai kem GIU LAI: dich vu nao vua hoi sinh va dang ky
     * nghe cung biet ngay he thong dang o tinh trang gi. */
    bool bad = (sev >= IPC_SEV_ERROR) || (act >= IPC_ACT_RESTART_SERVICE);
    if (bad != s_state.degraded) {
        s_state.degraded = bad;
        ipc_bus_publish_retained(TOPIC_HEALTH_STATE, bad ? 0 : 1, (int32_t)sev);
    }
}

static void install_rules(void)
{
    ipc_health_clear_rules();

    const ipc_health_rule_t rules[] = {
        /* Doc cam bien hong le te la binh thuong (nhieu, I2C ban). Chi khi
         * hong 3 lan trong 10 giay moi khoi dong lai dich vu cam bien.
         * Nguong nam O DAY chu khong trong svc_sensor.c: doc mot cho la biet
         * duoc luc nao dich vu se bi khoi dong lai. */
        { EXC_SENSOR_READ, SVC_SENSOR, IPC_SEV_WARN, 3, 10000,
          IPC_ACT_RESTART_SERVICE },

        /* Day len that bai la chuyen thuong khi khong co mang - uploader da
         * co hang doi va co che thu lai roi, nen chi ghi nhan. */
        { EXC_UPLOAD_FAIL, SVC_UPLOADER, IPC_SEV_WARN, 1, 0, IPC_ACT_LOG },

        /* Mat du lieu do hang doi tran phai thay duoc, nhung khong dong cham
         * gi vao he thong dang chay. */
        { EXC_QUEUE_OVERFLOW, NULL, IPC_SEV_WARN, 1, 0, IPC_ACT_LOG },

        /* Sap het bo nho: khoi dong lai dich vu ngon nhat truoc khi ca he
         * thong chet. */
        { IPC_EXC_OOM, NULL, IPC_SEV_ERROR, 1, 0, IPC_ACT_RESTART_SERVICE },

        /* Loi chet nguoi thi reboot. Luat rong nhat nen dat CUOI cung -
         * luat dau tien khop se thang. */
        { IPC_EXC_ANY, NULL, IPC_SEV_FATAL, 1, 0, IPC_ACT_REBOOT },
    };

    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); ++i)
        ipc_health_add_rule(&rules[i]);
}

/* ---------------- moc doi ---------------- */

static void health_on_create(app_service_t *self)
{
    const app_cfg_t *acfg = (const app_cfg_t *)self->ctx;
    self->state = &s_state;

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
}

static void health_on_subscribe(app_service_t *self)
{
    (void)self;
    ipc_bus_subscribe_service(TOPIC_ALERT, SVC_HEALTH, MSG_EV_ALERT);
}

static bool health_on_receive(app_service_t *self, ipc_message_t *msg)
{
    (void)self;
    if (msg->what == MSG_EV_ALERT) {
        /* Canh bao nguong khong phai loi he thong: dem lai de nhin xu huong,
         * khong khoi dong lai ai ca. */
        s_state.alerts_seen++;
    }
    return true;
}

/* ---------------- doc/ghi tham so ---------------- */

static int32_t health_get(app_service_t *self, uint32_t key, int32_t def)
{
    (void)self; (void)def;
    switch (key) {
    case HEALTHK_DEGRADED: return s_state.degraded ? 1 : 0;
    case HEALTHK_ALERTS:   return (int32_t)s_state.alerts_seen;
    default:               return SVC_VALUE_INVALID;
    }
}

static bool health_set(app_service_t *self, uint32_t key, int32_t value)
{
    (void)self;
    if (key != HEALTHK_DEGRADED || value != 0) return false;
    s_state.degraded = false;
    ipc_health_clear_degraded();
    return true;
}

/* ---------------- bang mo ta ---------------- */

static app_service_t s_svc = {
    .name = SVC_HEALTH,
    .priority = 8,
    .heartbeat_timeout_ms = 5000,
    .ready_bit = BIT_HEALTH_READY,
    .on_create = health_on_create,
    .on_subscribe = health_on_subscribe,
    .on_receive = health_on_receive,
    .get = health_get,
    .set = health_set,
};

app_service_t *svc_health(void) { return &s_svc; }
