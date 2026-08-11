/*
 * svc_uploader.c - gom du lieu thanh lo va day len server (mock).
 *
 * Day la dich vu duy nhat cham vao the gioi ben ngoai, nen no phai chiu
 * duoc viec the gioi ben ngoai khong dang tin:
 *   - mat mang    -> giu du lieu trong hang doi, khong vut di
 *   - loi tam thoi -> thu lai voi thoi gian cho tang dan
 *   - hang doi day -> bo ban ghi CU nhat va bao len health; du lieu moi
 *                     bao gio cung co gia tri hon du lieu cu trong do luong
 *
 * Cho y: khong co duong nao lam mat du lieu ma im lang. Moi lan mat deu
 * duoc dem va bao cao.
 */
#include "services.h"

#include "ipc_event.h"
#include "ipc_health.h"
#include "ipc_service.h"
#include "ipc_timer.h"

#include <stdio.h>

#define QUEUE_CAP 16
#define RETRY_BASE_MS 500
#define RETRY_MAX_MS 8000

static ipc_looper_t  *s_lp;
static ipc_handler_t  s_h;
static cloud_client_t *s_cloud;

static int32_t  s_queue[QUEUE_CAP];
static uint32_t s_head, s_count;
static uint32_t s_uploaded;      /* tong ban ghi da day thanh cong */
static uint32_t s_dropped;
static uint32_t s_retry_ms = RETRY_BASE_MS;
static ipc_timer_id_t s_retry_timer = IPC_TIMER_NONE;

static void queue_push(int32_t v)
{
    if (s_count == QUEUE_CAP) {
        /* Tran: bo ban ghi cu nhat. Bao len health de con nhin thay - mat
         * du lieu ma khong ai biet la kieu hong te nhat. */
        s_head = (s_head + 1) % QUEUE_CAP;
        s_count--;
        s_dropped++;
        ipc_health_report(SVC_UPLOADER, EXC_QUEUE_OVERFLOW, IPC_SEV_WARN,
                          (int32_t)s_dropped);
    }
    s_queue[(s_head + s_count) % QUEUE_CAP] = v;
    s_count++;
}

static void schedule_retry(void)
{
    if (s_retry_timer != IPC_TIMER_NONE) ipc_timer_destroy(s_retry_timer);
    s_retry_timer = ipc_timer_send_delayed(&s_h, MSG_UPLOAD_RETRY, 0, s_retry_ms);

    /* Thoi gian cho tang dan: mang hong thi dap cua lien tuc chi ton pin. */
    s_retry_ms *= 2;
    if (s_retry_ms > RETRY_MAX_MS) s_retry_ms = RETRY_MAX_MS;
}

static void flush(void)
{
    if (s_count == 0) return;
    if (!s_cloud->is_online(s_cloud)) { schedule_retry(); return; }

    char payload[256];
    char name[IPC_CFG_STR_LEN];
    ipc_cfg_get_str(CFG_DEVICE_NAME, name, sizeof(name), "unknown");

    int n = snprintf(payload, sizeof(payload), "dev=%s;", name);
    uint32_t packed = 0;
    for (uint32_t i = 0; i < s_count && n > 0 && (size_t)n < sizeof(payload); ++i) {
        int w = snprintf(payload + n, sizeof(payload) - (size_t)n, "%ld;",
                         (long)s_queue[(s_head + i) % QUEUE_CAP]);
        if (w < 0 || (size_t)w >= sizeof(payload) - (size_t)n) break;
        n += w;
        packed++;
    }

    if (s_cloud->upload(s_cloud, payload, (uint32_t)n) == 0) {
        /* Chi bo khoi hang doi nhung ban ghi THUC SU da day duoc. */
        s_head = (s_head + packed) % QUEUE_CAP;
        s_count -= packed;
        s_uploaded += packed;
        s_retry_ms = RETRY_BASE_MS;
        ipc_bus_publish(TOPIC_UPLOAD_RESULT, (int32_t)packed, 1);
        if (s_count > 0) flush();   /* con ton thi day tiep */
    } else {
        /* That bai: du lieu VAN nam trong hang doi, khong mat. */
        ipc_health_report(SVC_UPLOADER, EXC_UPLOAD_FAIL, IPC_SEV_WARN,
                          (int32_t)s_count);
        ipc_bus_publish(TOPIC_UPLOAD_RESULT, (int32_t)s_count, 0);
        schedule_retry();
    }
}

static bool uploader_cb(ipc_handler_t *h, ipc_message_t *m, void *user)
{
    (void)h; (void)user;
    switch (m->what) {
    case MSG_EV_DATA_READY: {
        queue_push(m->arg1);
        int32_t batch = ipc_cfg_get_int(CFG_UPLOAD_BATCH, 4);
        if (batch < 1) batch = 1;
        if (s_count >= (uint32_t)batch) flush();
        break;
    }

    case MSG_EV_NET_STATE:
        if (m->arg1) {
            ipc_event_group_set(app_bits(), BIT_NET_ONLINE);
            s_retry_ms = RETRY_BASE_MS;
            flush();               /* co mang lai thi day ngay phan ton dong */
        } else {
            ipc_event_group_clear(app_bits(), BIT_NET_ONLINE);
        }
        break;

    case MSG_UPLOAD_RETRY:
    case MSG_UPLOAD_FLUSH:
        s_retry_timer = IPC_TIMER_NONE;
        flush();
        break;

    default:
        break;
    }
    return true;
}

static void uploader_on_start(ipc_looper_t *lp, void *user)
{
    const app_cfg_t *acfg = (const app_cfg_t *)user;
    static cloud_mock_t s_default_cloud;
    if (acfg->cloud) {
        s_cloud = acfg->cloud;
    } else {
        cloud_mock_init(&s_default_cloud);
        s_cloud = &s_default_cloud.base;
    }

    ipc_bus_unsubscribe_service(SVC_UPLOADER);

    ipc_handler_init(&s_h, lp, uploader_cb, NULL, SVC_UPLOADER);
    ipc_service_register(SVC_UPLOADER, &s_h);

    ipc_bus_subscribe_service(TOPIC_DATA_READY, SVC_UPLOADER, MSG_EV_DATA_READY);
    ipc_bus_subscribe_service(TOPIC_NET_STATE, SVC_UPLOADER, MSG_EV_NET_STATE);

    /*
     * Hang doi KHONG bi xoa khi hoi sinh: du lieu chua day len la tai san,
     * task chet khong phai ly do de vut no di.
     */
    if (s_cloud->is_online(s_cloud))
        ipc_event_group_set(app_bits(), BIT_NET_ONLINE);

    ipc_event_group_set(app_bits(), BIT_UPLOADER_READY);
}

bool svc_uploader_start(const app_cfg_t *cfg)
{
    ipc_looper_cfg_t lc;
    ipc_looper_cfg_default(&lc, SVC_UPLOADER);
    lc.priority = 5;
    lc.heartbeat_timeout_ms = 6000;   /* rong hon: cham mang co the lau */
    lc.on_start = uploader_on_start;
    lc.user = (void *)cfg;

    s_lp = ipc_looper_create(&lc);
    if (!s_lp) return false;

    if (cfg->spawn_tasks) return ipc_looper_start(s_lp);
    uploader_on_start(s_lp, (void *)cfg);
    return true;
}

uint32_t svc_uploader_peek_pending(void)  { return s_count; }
uint32_t svc_uploader_peek_uploaded(void) { return s_uploaded; }
uint32_t svc_uploader_peek_dropped(void)  { return s_dropped; }
