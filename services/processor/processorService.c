/*
 * processorService.c - bien du lieu tho thanh du lieu dung duoc.
 *
 * Nghe TOPIC_SENSOR_SAMPLE, loc nhieu bang trung binh truot, cong bo
 * TOPIC_DATA_READY. Vuot nguong trong cau hinh thi cong bo them TOPIC_ALERT.
 *
 * Dich vu nay khong biet du lieu tu dau toi va cung khong biet ai dung ket
 * qua. Do la ly do co the cam them cam bien thu hai, hoac them man hinh nghe
 * TOPIC_DATA_READY, ma khong sua file nay.
 */
#include "app.h"
#include "processor/processorService.h"
#include "common/services.h"

#define WINDOW 4

typedef struct {
    int32_t  window[WINDOW];
    uint32_t filled;
    uint32_t pos;
    int32_t  last_avg;
    uint32_t alerts;
    uint32_t processed;
} proc_state_t;

static proc_state_t s_state;

static int32_t push_and_average(int32_t v)
{
    s_state.window[s_state.pos] = v;
    s_state.pos = (s_state.pos + 1) % WINDOW;
    if (s_state.filled < WINDOW) s_state.filled++;

    int64_t sum = 0;
    for (uint32_t i = 0; i < s_state.filled; ++i) sum += s_state.window[i];
    return (int32_t)(sum / (int32_t)s_state.filled);
}

static void check_thresholds(app_service_t *self, int32_t avg)
{
    int32_t hi = ipc_cfg_get_int(CFG_ALERT_HIGH, 30000);
    int32_t lo = ipc_cfg_get_int(CFG_ALERT_LOW, 15000);

    if (avg > hi) {
        s_state.alerts++;
        svc_publish_topic(self, TOPIC_ALERT, avg, 1);
    } else if (avg < lo) {
        s_state.alerts++;
        svc_publish_topic(self, TOPIC_ALERT, avg, -1);
    }
}

/* ---------------- moc doi ---------------- */

static void processor_on_create(app_service_t *self)
{
    self->state = &s_state;
    /*
     * Cua so loc CO Y khong bi xoa khi hoi sinh: du lieu do dac van con
     * hieu luc, chi co task la moi. Xoa di thi moi lan restart se sinh mot
     * buoc nhay gia trong du lieu.
     */
}

static void processor_on_subscribe(app_service_t *self)
{
    svc_listen_topic(self, TOPIC_SENSOR_SAMPLE, MSG_EV_SENSOR_SAMPLE);
}

/* ---------------- xu ly tung loai message ---------------- */

static bool on_sensor_sample(app_service_t *self, ipc_message_t *msg)
{
    s_state.last_avg = push_and_average(msg->arg1);
    s_state.processed++;
    svc_publish_topic(self, TOPIC_DATA_READY, s_state.last_avg,
                      (int32_t)s_state.processed);
    check_thresholds(self, s_state.last_avg);
    return true;
}

static const svc_route_t k_routes[] = {
    { MSG_EV_SENSOR_SAMPLE, on_sensor_sample, "sensor_sample" },
};

/* ---------------- doc/ghi tham so ---------------- */

static int32_t processor_get(app_service_t *self, uint32_t key, int32_t def)
{
    (void)self; (void)def;
    switch (key) {
    case PROCK_LAST_AVG:  return s_state.last_avg;
    case PROCK_ALERTS:    return (int32_t)s_state.alerts;
    case PROCK_PROCESSED: return (int32_t)s_state.processed;
    default:              return SVC_VALUE_INVALID;
    }
}

static bool processor_set(app_service_t *self, uint32_t key, int32_t value)
{
    (void)self; (void)key; (void)value;
    return false;   /* nguong nam trong cau hinh, doi qua configService */
}

/* ---------------- bang mo ta ---------------- */

static app_service_t s_svc = {
    .name = SVC_PROCESSOR,
    .priority = 6,
    .heartbeat_timeout_ms = 4000,
    .ready_bit = BIT_PROCESSOR_READY,
    .on_create = processor_on_create,
    .on_subscribe = processor_on_subscribe,
    .routes = k_routes,
    .route_count = sizeof(k_routes) / sizeof(k_routes[0]),
    .get = processor_get,
    .set = processor_set,
};

app_service_t *processorService(void) { return &s_svc; }
