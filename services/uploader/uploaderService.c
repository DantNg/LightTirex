/*
 * uploaderService.c - gom du lieu thanh lo va day len server (mock).
 *
 * Day la dich vu duy nhat cham vao the gioi ben ngoai, nen phai chiu duoc
 * viec the gioi ben ngoai khong dang tin:
 *   - mat mang     -> giu du lieu trong hang doi, khong vut di
 *   - loi tam thoi -> thu lai voi thoi gian cho tang dan
 *   - hang doi day -> bo ban ghi CU nhat va bao len health; trong do luong,
 *                     du lieu moi co gia tri hon du lieu cu
 *
 * Khong co duong nao lam mat du lieu ma im lang: moi lan mat deu duoc dem
 * va bao cao.
 */
#include "app.h"
#include "uploader/uploaderService.h"
#include "common/services.h"

#include "ipc_event.h"
#include "ipc_health.h"
#include "ipc_service.h"
#include "ipc_timer.h"

#include <stdio.h>
#include <string.h>

#define QUEUE_CAP     16
#define RETRY_BASE_MS 500
#define RETRY_MAX_MS  8000

typedef struct {
    cloud_client_t *cloud;
    int32_t  queue[QUEUE_CAP];
    uint32_t head, count;
    uint32_t uploaded;
    uint32_t dropped;
    uint32_t retry_ms;
    ipc_timer_id_t retry_timer;
} upload_state_t;

static upload_state_t s_state = { .retry_ms = RETRY_BASE_MS,
                                  .retry_timer = IPC_TIMER_NONE };

static void queue_push(int32_t v)
{
    if (s_state.count == QUEUE_CAP) {
        /* Tran: bo ban ghi cu nhat. Bao len health de con nhin thay - mat
         * du lieu ma khong ai biet la kieu hong te nhat. */
        s_state.head = (s_state.head + 1) % QUEUE_CAP;
        s_state.count--;
        s_state.dropped++;
        ipc_health_report(SVC_UPLOADER, EXC_QUEUE_OVERFLOW, IPC_SEV_WARN,
                          (int32_t)s_state.dropped);
    }
    s_state.queue[(s_state.head + s_state.count) % QUEUE_CAP] = v;
    s_state.count++;
}

static void schedule_retry(void)
{
    if (s_state.retry_timer != IPC_TIMER_NONE)
        ipc_timer_destroy(s_state.retry_timer);

    /* Tra cuu handler theo TEN chu khong giu con tro: dich vu co the vua
     * duoc hoi sinh voi handler khac. */
    ipc_handler_t *self = ipc_service_get(SVC_UPLOADER);
    if (!self) return;
    s_state.retry_timer = ipc_timer_send_delayed(self, MSG_UPLOAD_RETRY,
                                                 0, s_state.retry_ms);

    /* Thoi gian cho tang dan: mang hong ma dap cua lien tuc thi chi ton pin. */
    s_state.retry_ms *= 2;
    if (s_state.retry_ms > RETRY_MAX_MS) s_state.retry_ms = RETRY_MAX_MS;
}

static void flush(void)
{
    if (s_state.count == 0) return;
    if (!s_state.cloud->is_online(s_state.cloud)) { schedule_retry(); return; }

    char payload[256];
    char name[IPC_CFG_STR_LEN];
    ipc_cfg_get_str(CFG_DEVICE_NAME, name, sizeof(name), "unknown");

    int n = snprintf(payload, sizeof(payload), "dev=%s;", name);
    uint32_t packed = 0;
    for (uint32_t i = 0; i < s_state.count && n > 0 && (size_t)n < sizeof(payload); ++i) {
        int w = snprintf(payload + n, sizeof(payload) - (size_t)n, "%ld;",
                         (long)s_state.queue[(s_state.head + i) % QUEUE_CAP]);
        if (w < 0 || (size_t)w >= sizeof(payload) - (size_t)n) break;
        n += w;
        packed++;
    }

    if (s_state.cloud->upload(s_state.cloud, payload, (uint32_t)n) == 0) {
        /* Chi bo khoi hang doi nhung ban ghi THUC SU da day duoc. */
        s_state.head = (s_state.head + packed) % QUEUE_CAP;
        s_state.count -= packed;
        s_state.uploaded += packed;
        s_state.retry_ms = RETRY_BASE_MS;
        ipc_bus_publish(TOPIC_UPLOAD_RESULT, (int32_t)packed, 1);
        if (s_state.count > 0) flush();   /* con ton thi day tiep */
    } else {
        /* That bai: du lieu VAN nam trong hang doi, khong mat. */
        ipc_health_report(SVC_UPLOADER, EXC_UPLOAD_FAIL, IPC_SEV_WARN,
                          (int32_t)s_state.count);
        ipc_bus_publish(TOPIC_UPLOAD_RESULT, (int32_t)s_state.count, 0);
        schedule_retry();
    }
}

/* ---------------- moc doi ---------------- */

static void uploader_on_create(app_service_t *self)
{
    const app_cfg_t *acfg = (const app_cfg_t *)self->ctx;
    static cloud_mock_t s_default_cloud;

    if (acfg->cloud) {
        s_state.cloud = acfg->cloud;
    } else {
        cloud_mock_init(&s_default_cloud);
        s_state.cloud = &s_default_cloud.base;
    }
    self->state = &s_state;

    /*
     * Hang doi CO Y khong bi xoa khi hoi sinh: du lieu chua day len la tai
     * san, task chet khong phai ly do de vut no di.
     */
    if (s_state.cloud->is_online(s_state.cloud))
        ipc_event_group_set(app_bits(), BIT_NET_ONLINE);
}

static void uploader_on_subscribe(app_service_t *self)
{
    (void)self;
    ipc_bus_subscribe_service(TOPIC_DATA_READY, SVC_UPLOADER, MSG_EV_DATA_READY);
    ipc_bus_subscribe_service(TOPIC_NET_STATE, SVC_UPLOADER, MSG_EV_NET_STATE);
}

/* ---------------- xu ly tung loai message ---------------- */

/* Co du lieu moi: xep vao hang doi, du lo thi day di. */
static bool on_data_ready(app_service_t *self, ipc_message_t *msg)
{
    (void)self;
    queue_push(msg->arg1);

    int32_t batch = ipc_cfg_get_int(CFG_UPLOAD_BATCH, 4);
    if (batch < 1) batch = 1;
    if (s_state.count >= (uint32_t)batch) flush();
    return true;
}

/* Mang len hoac xuong. */
static bool on_net_state(app_service_t *self, ipc_message_t *msg)
{
    (void)self;
    if (msg->arg1) {
        ipc_event_group_set(app_bits(), BIT_NET_ONLINE);
        s_state.retry_ms = RETRY_BASE_MS;
        flush();                   /* co mang lai thi day ngay phan ton dong */
    } else {
        ipc_event_group_clear(app_bits(), BIT_NET_ONLINE);
    }
    return true;
}

/* Den han thu lai, hoac bi thuc ep day. */
static bool on_flush(app_service_t *self, ipc_message_t *msg)
{
    (void)self; (void)msg;
    s_state.retry_timer = IPC_TIMER_NONE;
    flush();
    return true;
}

static const svc_route_t k_routes[] = {
    { MSG_EV_DATA_READY, on_data_ready, "data_ready" },
    { MSG_EV_NET_STATE,  on_net_state,  "net_state"  },
    { MSG_UPLOAD_RETRY,  on_flush,      "retry"      },
    { MSG_UPLOAD_FLUSH,  on_flush,      "flush"      },
};

/* ---------------- doc/ghi tham so ---------------- */

static int32_t uploader_get(app_service_t *self, uint32_t key, int32_t def)
{
    (void)self; (void)def;
    switch (key) {
    case UPK_PENDING:  return (int32_t)s_state.count;
    case UPK_UPLOADED: return (int32_t)s_state.uploaded;
    case UPK_DROPPED:  return (int32_t)s_state.dropped;
    case UPK_ONLINE:   return s_state.cloud &&
                              s_state.cloud->is_online(s_state.cloud) ? 1 : 0;
    default:           return SVC_VALUE_INVALID;
    }
}

static bool uploader_set(app_service_t *self, uint32_t key, int32_t value)
{
    (void)self;
    if (key != UPK_ONLINE) return false;
    /* Bao co mang lai la mot su kien he thong, di qua bus de ai quan tam
     * cung biet - khong sua lut trang thai ben trong. */
    ipc_bus_publish(TOPIC_NET_STATE, value ? 1 : 0, 0);
    return true;
}

/* ---------------- bang mo ta ---------------- */

static app_service_t s_svc = {
    .name = SVC_UPLOADER,
    .priority = 5,
    .heartbeat_timeout_ms = 6000,   /* rong hon: cham mang co the lau */
    .ready_bit = BIT_UPLOADER_READY,
    .on_create = uploader_on_create,
    .on_subscribe = uploader_on_subscribe,
    .routes = k_routes,
    .route_count = sizeof(k_routes) / sizeof(k_routes[0]),
    .get = uploader_get,
    .set = uploader_set,
};

app_service_t *uploaderService(void) { return &s_svc; }
