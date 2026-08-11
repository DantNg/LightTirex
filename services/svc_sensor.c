/*
 * svc_sensor.c - doc cam bien theo chu ky va cong bo mau len bus.
 *
 * Hai diem dang chu y:
 *
 * 1. Timer duoc dat kieu TO_SERVICE (theo TEN) chu khong theo con tro
 *    handler. Dich vu nay chet va duoc hoi sinh thi handler la mot con tro
 *    khac, nhung nhip lay mau van chay tiep khong dut.
 *
 * 2. No la NGUOI NGHE cua cau hinh: doi chu ky lay mau trong file config thi
 *    su kien TOPIC_CONFIG_CHANGED se toi day va timer duoc dat lai. Khong
 *    can khoi dong lai dich vu, va svc_config khong he biet svc_sensor ton tai.
 */
#include "services.h"

#include "ipc_event.h"
#include "ipc_health.h"
#include "ipc_service.h"
#include "ipc_timer.h"

static ipc_looper_t     *s_lp;
static ipc_handler_t     s_h;
static sensor_driver_t  *s_drv;
static int32_t           s_seq;
static uint32_t          s_fail_streak;
static uint32_t          s_total_fails;

static void arm_sampling_timer(void)
{
    /* Huy nhip cu truoc: on_start chay lai sau moi lan hoi sinh, khong don
     * thi se co hai timer cung ban. */
    ipc_timer_cancel_for_service(SVC_SENSOR);

    int32_t period = ipc_cfg_get_int(CFG_SAMPLE_PERIOD, 1000);
    if (period < 10) period = 10;   /* chan duoi: khong de cau hinh sai lam
                                     * chet he thong bang mot vong ban lien tuc */
    ipc_timer_send_periodic_to(SVC_SENSOR, MSG_SENSOR_TICK, (uint32_t)period);
}

static void do_sample(void)
{
    int32_t mc = 0;
    if (s_drv->read(s_drv, &mc)) {
        s_fail_streak = 0;
        ipc_bus_publish(TOPIC_SENSOR_SAMPLE, mc, s_seq++);
        return;
    }

    /*
     * Doc hong: bao SU VIEC len health, khong tu quyet dinh gi ca.
     *
     * Co y khong tu leo thang muc do theo so lan hong lien tiep o day: quyet
     * dinh "bao nhieu lan trong bao lau thi coi la hong that" la viec cua
     * bang luat trong svc_health.c. Neu lam ca hai noi thi nguong bi chia doi
     * va khong ai doc code doan duoc luc nao dich vu se bi khoi dong lai.
     * s_fail_streak chi di theo nhu so lieu chan doan.
     */
    s_fail_streak++;
    s_total_fails++;
    ipc_health_report(SVC_SENSOR, EXC_SENSOR_READ, IPC_SEV_WARN,
                      (int32_t)s_fail_streak);
}

static bool sensor_cb(ipc_handler_t *h, ipc_message_t *m, void *user)
{
    (void)h; (void)user;
    switch (m->what) {
    case MSG_SENSOR_TICK:
        do_sample();
        break;

    case MSG_EV_CONFIG_CHANGED:
        if (m->arg1 == CFGK_SAMPLE_PERIOD_MS) arm_sampling_timer();
        break;

    default:
        break;
    }
    return true;
}

static void sensor_on_start(ipc_looper_t *lp, void *user)
{
    const app_cfg_t *acfg = (const app_cfg_t *)user;
    s_drv = acfg->sensor ? acfg->sensor : sensor_driver_sim();

    ipc_bus_unsubscribe_service(SVC_SENSOR);

    ipc_handler_init(&s_h, lp, sensor_cb, NULL, SVC_SENSOR);
    ipc_service_register(SVC_SENSOR, &s_h);

    ipc_bus_subscribe_service(TOPIC_CONFIG_CHANGED, SVC_SENSOR,
                              MSG_EV_CONFIG_CHANGED);

    s_fail_streak = 0;
    arm_sampling_timer();
    ipc_event_group_set(app_bits(), BIT_SENSOR_READY);
}

bool svc_sensor_start(const app_cfg_t *cfg)
{
    ipc_looper_cfg_t lc;
    ipc_looper_cfg_default(&lc, SVC_SENSOR);
    lc.priority = 7;
    lc.heartbeat_timeout_ms = 4000;
    lc.max_restarts = 10;
    lc.restart_backoff_ms = 0;
    lc.on_start = sensor_on_start;
    lc.user = (void *)cfg;

    s_lp = ipc_looper_create(&lc);
    if (!s_lp) return false;

    if (cfg->spawn_tasks) return ipc_looper_start(s_lp);
    sensor_on_start(s_lp, (void *)cfg);
    return true;
}

int32_t  svc_sensor_peek_samples(void) { return s_seq; }
uint32_t svc_sensor_peek_fails(void)   { return s_total_fails; }
