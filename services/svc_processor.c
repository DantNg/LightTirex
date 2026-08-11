/*
 * svc_processor.c - xu ly du lieu tho thanh du lieu dung duoc.
 *
 * Nghe TOPIC_SENSOR_SAMPLE, loc nhieu bang trung binh truot, roi cong bo
 * TOPIC_DATA_READY. Neu gia tri vuot nguong trong cau hinh thi cong bo them
 * TOPIC_ALERT.
 *
 * Dich vu nay khong biet du lieu tu dau toi va cung khong biet ai se dung
 * ket qua. Do la ly do co the cam them mot cam bien thu hai, hoac them mot
 * man hinh hien thi nghe TOPIC_DATA_READY, ma khong sua file nay.
 */
#include "services.h"

#include "ipc_event.h"
#include "ipc_service.h"

#define WINDOW 4

static ipc_looper_t *s_lp;
static ipc_handler_t s_h;

static int32_t  s_window[WINDOW];
static uint32_t s_filled;
static uint32_t s_pos;
static int32_t  s_last_avg;
static uint32_t s_alerts;
static uint32_t s_processed;

static int32_t push_and_average(int32_t v)
{
    s_window[s_pos] = v;
    s_pos = (s_pos + 1) % WINDOW;
    if (s_filled < WINDOW) s_filled++;

    int64_t sum = 0;
    for (uint32_t i = 0; i < s_filled; ++i) sum += s_window[i];
    return (int32_t)(sum / (int32_t)s_filled);
}

static void check_thresholds(int32_t avg)
{
    int32_t hi = ipc_cfg_get_int(CFG_ALERT_HIGH, 30000);
    int32_t lo = ipc_cfg_get_int(CFG_ALERT_LOW, 15000);

    if (avg > hi) {
        s_alerts++;
        ipc_bus_publish(TOPIC_ALERT, avg, 1);
    } else if (avg < lo) {
        s_alerts++;
        ipc_bus_publish(TOPIC_ALERT, avg, -1);
    }
}

static bool processor_cb(ipc_handler_t *h, ipc_message_t *m, void *user)
{
    (void)h; (void)user;
    if (m->what != MSG_EV_SENSOR_SAMPLE) return true;

    s_last_avg = push_and_average(m->arg1);
    s_processed++;
    ipc_bus_publish(TOPIC_DATA_READY, s_last_avg, (int32_t)s_processed);
    check_thresholds(s_last_avg);
    return true;
}

static void processor_on_start(ipc_looper_t *lp, void *user)
{
    (void)user;
    ipc_bus_unsubscribe_service(SVC_PROCESSOR);

    ipc_handler_init(&s_h, lp, processor_cb, NULL, SVC_PROCESSOR);
    ipc_service_register(SVC_PROCESSOR, &s_h);
    ipc_bus_subscribe_service(TOPIC_SENSOR_SAMPLE, SVC_PROCESSOR,
                              MSG_EV_SENSOR_SAMPLE);

    /*
     * Cua so loc KHONG duoc xoa khi hoi sinh: du lieu do dac van con hieu
     * luc, chi co task la moi. Neu xoa, moi lan restart se sinh mot buoc
     * nhay gia trong du lieu.
     */
    ipc_event_group_set(app_bits(), BIT_PROCESSOR_READY);
}

bool svc_processor_start(const app_cfg_t *cfg)
{
    ipc_looper_cfg_t lc;
    ipc_looper_cfg_default(&lc, SVC_PROCESSOR);
    lc.priority = 6;
    lc.heartbeat_timeout_ms = 4000;
    lc.on_start = processor_on_start;

    s_lp = ipc_looper_create(&lc);
    if (!s_lp) return false;

    if (cfg->spawn_tasks) return ipc_looper_start(s_lp);
    processor_on_start(s_lp, NULL);
    return true;
}

int32_t  svc_processor_peek_avg(void)    { return s_last_avg; }
uint32_t svc_processor_peek_alerts(void) { return s_alerts; }
