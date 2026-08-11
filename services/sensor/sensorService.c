/*
 * sensorService.c - doc cam bien theo chu ky, cong bo mau len bus.
 *
 * Hai diem dang chu y:
 *
 * 1. Nhip lay mau dat kieu TO_SERVICE (theo TEN) chu khong theo con tro
 *    handler. Dich vu chet va duoc hoi sinh thi handler la con tro khac,
 *    nhung nhip van chay tiep khong dut.
 *
 * 2. No la NGUOI NGHE cua cau hinh: doi chu ky lay mau trong file thi su
 *    kien TOPIC_CONFIG_CHANGED toi day va timer duoc dat lai. Khong can khoi
 *    dong lai dich vu, va configService khong he biet sensorService ton tai.
 */
#include "app.h"
#include "sensor/sensorService.h"
#include "common/services.h"

#include "ipc_event.h"
#include "ipc_health.h"
#include "ipc_timer.h"

typedef struct {
    sensor_driver_t *drv;
    int32_t  samples;
    uint32_t fails;
    uint32_t fail_streak;
} sensor_state_t;

static sensor_state_t s_state;

static void arm_sampling_timer(void)
{
    /* Huy nhip cu truoc: on_create chay lai sau moi lan hoi sinh, khong don
     * thi se co hai timer cung ban. */
    ipc_timer_cancel_for_service(SVC_SENSOR);

    int32_t period = ipc_cfg_get_int(CFG_SAMPLE_PERIOD, 1000);
    if (period < 10) period = 10;   /* chan duoi: cau hinh sai khong duoc
                                     * bien thanh vong ban lien tuc */
    ipc_timer_send_periodic_to(SVC_SENSOR, MSG_SENSOR_TICK, (uint32_t)period);
}

static void do_sample(void)
{
    int32_t mc = 0;
    if (s_state.drv->read(s_state.drv, &mc)) {
        s_state.fail_streak = 0;
        ipc_bus_publish(TOPIC_SENSOR_SAMPLE, mc, s_state.samples++);
        return;
    }

    /*
     * Doc hong: bao SU VIEC len health, khong tu quyet dinh gi.
     *
     * Co y khong tu leo thang muc do theo so lan hong lien tiep: quyet dinh
     * "bao nhieu lan trong bao lau thi coi la hong that" la viec cua bang
     * luat trong healthService.c. Lam ca hai noi thi nguong bi chia doi va
     * khong ai doc code doan duoc luc nao dich vu se bi khoi dong lai.
     */
    s_state.fail_streak++;
    s_state.fails++;
    ipc_health_report(SVC_SENSOR, EXC_SENSOR_READ, IPC_SEV_WARN,
                      (int32_t)s_state.fail_streak);
}

/* ---------------- moc doi ---------------- */

static void sensor_on_create(app_service_t *self)
{
    const app_cfg_t *acfg = (const app_cfg_t *)self->ctx;
    s_state.drv = acfg->sensor ? acfg->sensor : sensor_driver_sim();
    s_state.fail_streak = 0;
    self->state = &s_state;

    arm_sampling_timer();
}

static void sensor_on_subscribe(app_service_t *self)
{
    (void)self;
    ipc_bus_subscribe_service(TOPIC_CONFIG_CHANGED, SVC_SENSOR,
                              MSG_EV_CONFIG_CHANGED);
}

/* ---------------- xu ly tung loai message ---------------- */

/* Den nhip lay mau. */
static bool on_tick(app_service_t *self, ipc_message_t *msg)
{
    (void)self; (void)msg;
    do_sample();
    return true;
}

/* Cau hinh doi -> dat lai nhip. Khong can khoi dong lai dich vu. */
static bool on_config_changed(app_service_t *self, ipc_message_t *msg)
{
    (void)self;
    if (msg->arg1 == CFGK_PERIOD_MS) arm_sampling_timer();
    return true;
}

static const svc_route_t k_routes[] = {
    { MSG_SENSOR_TICK,       on_tick,           "tick"           },
    { MSG_EV_CONFIG_CHANGED, on_config_changed, "config_changed" },
};

/* ---------------- doc/ghi tham so ---------------- */

static int32_t sensor_get(app_service_t *self, uint32_t key, int32_t def)
{
    (void)self; (void)def;
    switch (key) {
    case SENSORK_SAMPLES:     return s_state.samples;
    case SENSORK_FAILS:       return (int32_t)s_state.fails;
    case SENSORK_FAIL_STREAK: return (int32_t)s_state.fail_streak;
    default:                  return SVC_VALUE_INVALID;
    }
}

/* Chu ky lay mau la cau hinh cua he thong, khong phai tham so rieng cua
 * cam bien - doi no thi doi qua configService de moi nguoi cung biet. */
static bool sensor_set(app_service_t *self, uint32_t key, int32_t value)
{
    (void)self; (void)key; (void)value;
    return false;
}

/* ---------------- bang mo ta ---------------- */

static app_service_t s_svc = {
    .name = SVC_SENSOR,
    .priority = 7,
    .heartbeat_timeout_ms = 4000,
    .ready_bit = BIT_SENSOR_READY,
    .on_create = sensor_on_create,
    .on_subscribe = sensor_on_subscribe,
    .routes = k_routes,
    .route_count = sizeof(k_routes) / sizeof(k_routes[0]),
    .get = sensor_get,
    .set = sensor_set,
};

app_service_t *sensorService(void) { return &s_svc; }
