/*
 * ipc_event.c - Event bus: dieu phoi su kien theo mo hinh publish/subscribe.
 *
 * ============================ TOAN CANH ============================
 *
 * Bus KHONG co task rieng. Toan bo cong viec dieu phoi chay tren task cua
 * NGUOI CONG BO, va chi gom hai viec: tra bang xem ai dang nghe, roi bo mot
 * message vao hang doi cua tung nguoi nghe. Bus khong bao gio goi callback
 * cua nguoi nghe - do la ly do mot nguoi nghe cham khong the lam nghen nguoi
 * cong bo.
 *
 * Mot lan publish di qua bon giai doan:
 *
 *   1. chup danh sach nguoi nghe   (giu khoa bus, O(so slot dang ky))
 *   2. tha khoa bus                 <-- bat buoc, xem ghi chu ve deadlock duoi
 *   3. voi TUNG nguoi nghe:
 *        phan giai handler -> lay mot message tu pool -> chen vao hang doi
 *   4. task cua nguoi nghe tu thuc day va xu ly sau
 *
 * ====================== CHI PHI CAN BIET TRUOC ======================
 *
 *   - MOI nguoi nghe ton MOT message rieng tu pool. Mot su kien toi 3 nguoi
 *     nghe = 3 slot pool. Pool la tai nguyen dung chung toan he thong.
 *   - Mang chup nguoi nghe nam tren STACK cua task cong bo:
 *     IPC_MAX_SUBSCRIPTIONS * sizeof(subscription_t) ~ 1.1 KB voi 32 slot
 *     tren MCU 32-bit. Task cong bo phai co stack du rong.
 *
 * ========================= THU TU LAY KHOA =========================
 *
 * Chi co MOT thu tu hop le: bus -> (tha bus) -> looper dich.
 *
 * Khong bao gio duoc giu khoa bus trong luc chen vao hang doi looper, vi
 * duong nguoc lai co that: mot handler dang chay (task giu khoa looper cua
 * minh) hoan toan co the goi ipc_bus_subscribe(). Neu publish giu bus roi
 * doi looper, con ben kia giu looper roi doi bus, hai ben khoa chet nhau.
 * Vi vay giai doan 1 chup ra ban sao roi tha khoa ngay.
 */
#include "ipc_event.h"
#include "ipc_service.h"

#include <stdio.h>
#include <string.h>

/* Thoi gian toi da cho khoa bus. Qua han nghia la co gi do rat sai (chu so
 * huu khoa da chet), khi do ta bo qua thao tac chu khong treo vo han. */
#define BUS_LOCK_TIMEOUT_MS 1000u

/*
 * Mot ban dang ky nghe.
 *
 * Co hai cach tro toi nguoi nghe, va lua chon nay quyet dinh hanh vi khi
 * dich vu chet roi hoi sinh:
 *
 *   resolve_by_name = false -> dung `handler` truc tiep. Nhanh, nhung neu
 *        dich vu hoi sinh thi handler cu khong con hop le -> mat su kien.
 *        Chi dung cho nguoi nghe khong bao gio khoi dong lai.
 *
 *   resolve_by_name = true  -> tra ServiceManager theo `service_name` tai
 *        DUNG luc cong bo. Dich vu hoi sinh voi handler moi van nhan duoc.
 *        Day la cach moi dich vu trong he thong nay dung.
 */
typedef struct {
    uint32_t topic;                          /* chu de dang nghe */
    uint32_t deliver_as_what;                /* ma `what` se dien vao message */

    ipc_handler_t *handler;                  /* dung khi resolve_by_name = false */
    char           service_name[IPC_BUS_NAME_LEN]; /* dung khi resolve_by_name = true */
    bool           resolve_by_name;

    bool     in_use;
    uint16_t generation;                     /* tang moi lan slot duoc cap lai */
    uint16_t slot_index;                     /* vi tri trong s_subscriptions[] */
} subscription_t;

/*
 * Gia tri cuoi cung cua mot chu de, giu lai de nguoi dang ky SAU van biet
 * trang thai hien tai ma khong phai cho nhip cap nhat ke tiep.
 *
 * Chi dung cho TRANG THAI (mang len/xuong, suc khoe he thong). Khong dung
 * cho du lieu do dac: giao lai mot mau cu cho nguoi moi vao la bia du lieu.
 */
typedef struct {
    uint32_t    topic;
    ipc_event_t last_event;
    bool        in_use;
} retained_event_t;

static subscription_t   s_subscriptions[IPC_MAX_SUBSCRIPTIONS];
static retained_event_t s_retained_events[IPC_MAX_RETAINED_TOPICS];
static ipc_mutex_t      s_bus_lock;
static ipc_bus_stats_t  s_bus_stats;

/*
 * Khoi tao bus. An toan khi goi nhieu lan - moi duong vao cong khai deu goi
 * ham nay nen nguoi dung khong bat buoc phai nho goi truoc.
 */
void ipc_bus_init(void)
{
    if (s_bus_lock) return;
    ipc_message_pool_init();
    s_bus_lock = ipc_mutex_create();
}

/* ================================================================== */
/* Quan ly slot dang ky                                               */
/* ================================================================== */

/*
 * Ghep id cong khai tu (generation, slot).
 *
 *   bit 31..16 : generation - tang moi lan slot duoc cap phat lai
 *   bit 15..0  : slot_index + 1 - cong 1 de id khong bao gio bang 0,
 *                vi 0 la IPC_SUB_NONE
 *
 * Generation la thu chan loi ABA: mot id cu tro toi slot da duoc cap lai
 * cho nguoi khac se KHONG phan giai duoc, nen huy nham la khong the.
 */
static ipc_sub_id_t subscription_make_id(const subscription_t *sub)
{
    return ((ipc_sub_id_t)sub->generation << 16) |
           (ipc_sub_id_t)(sub->slot_index + 1);
}

/*
 * Nguoc lai cua subscription_make_id: id -> ban dang ky, hoac NULL neu id
 * khong con hop le (da huy, hoac slot da thuoc ve nguoi khac).
 *
 * Nguoi goi phai dang giu s_bus_lock.
 */
static subscription_t *subscription_from_id(ipc_sub_id_t id)
{
    if (id == IPC_SUB_NONE) return NULL;

    uint16_t slot_index = (uint16_t)((id & 0xFFFFu) - 1);
    uint16_t generation = (uint16_t)(id >> 16);
    if (slot_index >= IPC_MAX_SUBSCRIPTIONS) return NULL;

    subscription_t *sub = &s_subscriptions[slot_index];
    if (!sub->in_use) return NULL;
    if (sub->generation != generation) return NULL;   /* slot da doi chu */
    return sub;
}

/*
 * Cap mot slot dang ky trong. Tra NULL neu het slot.
 *
 * Luu y thu tu: doc generation cu ra TRUOC khi memset, roi tang len va ghi
 * lai. Neu memset xoa generation ve 0 thi id cu se phan giai trung voi id
 * moi - dung cai loi ABA ma generation sinh ra de chan.
 *
 * Nguoi goi phai dang giu s_bus_lock.
 */
static subscription_t *subscription_alloc(uint32_t topic, uint32_t deliver_as_what)
{
    for (int i = 0; i < IPC_MAX_SUBSCRIPTIONS; ++i) {
        if (s_subscriptions[i].in_use) continue;

        subscription_t *sub = &s_subscriptions[i];
        uint16_t next_generation = (uint16_t)(sub->generation + 1);

        memset(sub, 0, sizeof(*sub));
        sub->slot_index = (uint16_t)i;
        sub->generation = next_generation;
        sub->topic = topic;
        sub->deliver_as_what = deliver_as_what;
        sub->in_use = true;
        return sub;
    }
    return NULL;
}

/*
 * Tim gia tri giu lai cua mot chu de. NULL neu chu de do chua tung duoc
 * cong bo kem giu lai.
 *
 * Nguoi goi phai dang giu s_bus_lock.
 */
static retained_event_t *retained_find(uint32_t topic)
{
    for (int i = 0; i < IPC_MAX_RETAINED_TOPICS; ++i) {
        if (s_retained_events[i].in_use && s_retained_events[i].topic == topic)
            return &s_retained_events[i];
    }
    return NULL;
}

/* ================================================================== */
/* Giao mot su kien cho MOT nguoi nghe                                */
/* ================================================================== */

/*
 * Bo mot message vao hang doi cua mot nguoi nghe.
 *
 * Ten ham co chu "enqueue" chu khong phai "deliver" hay "notify" la co y:
 * ham nay KHONG chay code cua nguoi nghe. No chi day message vao hang doi
 * roi tra ve ngay. Nguoi nghe xu ly sau, tren task cua chinh no.
 *
 * PHAI chay NGOAI vung khoa bus: ben trong co lay khoa cua looper dich va
 * khoa cua ServiceManager (xem ghi chu thu tu lay khoa o dau file).
 *
 * Tra false trong ba truong hop, tat ca deu duoc dem vao thong ke `dropped`
 * chu khong bao gio im lang:
 *   1. dang ky theo ten nhung dich vu khong con trong ServiceManager
 *      (dang chet, hoac da bi giet han)
 *   2. pool message da can
 *   3. looper dich da dung (ipc_handler_send tu tu choi)
 */
static bool enqueue_to_subscriber(const subscription_t *sub,
                                  const ipc_event_t *event)
{
    /* Phan giai theo ten tai DUNG thoi diem cong bo, khong phai luc dang ky:
     * dich vu vua duoc hoi sinh se co handler moi va van nhan duoc su kien. */
    ipc_handler_t *target = sub->resolve_by_name
                                ? ipc_service_get(sub->service_name)
                                : sub->handler;
    if (!target) return false;

    ipc_message_t *msg = ipc_message_obtain();
    if (!msg) return false;   /* pool can - nguoi goi se dem vao dropped */

    msg->what        = sub->deliver_as_what;   /* ma do NGUOI NGHE chon */
    msg->topic       = event->topic;           /* de phan biet neu nghe nhieu chu de */
    msg->arg1        = event->arg1;
    msg->arg2        = event->arg2;
    msg->payload     = event->payload;
    msg->payload_len = event->payload_len;
    /* payload_free co tinh de NULL: bus khong so huu payload. Mot su kien
     * toi nhieu nguoi nghe nen "ai free" khong bao gio ro rang, vi vay bus
     * tu choi trach nhiem do thay vi doan. */

    return ipc_handler_send(target, msg);
}

/* ================================================================== */
/* Dang ky nghe                                                       */
/* ================================================================== */

/*
 * Phan chung cua hai kieu dang ky.
 *
 * Ngoai viec cap slot, ham con lo mot viec de bi quen: neu chu de da co gia
 * tri giu lai thi giao ngay cho nguoi vua dang ky. Viec giao do phai lam
 * NGOAI vung khoa bus, nen ta chup ban sao ban dang ky va su kien roi moi
 * tha khoa.
 *
 * handler != NULL  -> dang ky theo con tro
 * service != NULL  -> dang ky theo ten
 */
static ipc_sub_id_t subscribe_common(uint32_t topic, ipc_handler_t *handler,
                                     const char *service,
                                     uint32_t deliver_as_what)
{
    ipc_bus_init();
    if (!ipc_mutex_lock(s_bus_lock, BUS_LOCK_TIMEOUT_MS)) return IPC_SUB_NONE;

    subscription_t *sub = subscription_alloc(topic, deliver_as_what);
    if (!sub) {
        ipc_mutex_unlock(s_bus_lock);
        return IPC_SUB_NONE;   /* het slot dang ky */
    }

    if (service) {
        sub->resolve_by_name = true;
        snprintf(sub->service_name, sizeof(sub->service_name), "%s", service);
    } else {
        sub->handler = handler;
    }

    ipc_sub_id_t id = subscription_make_id(sub);

    /* Chup lai de giao ngoai vung khoa. */
    retained_event_t *retained = retained_find(topic);
    subscription_t sub_copy;
    ipc_event_t    retained_copy;
    bool           has_retained = (retained != NULL);
    if (has_retained) {
        sub_copy = *sub;
        retained_copy = retained->last_event;
    }

    ipc_mutex_unlock(s_bus_lock);

    /* Nguoi dang ky muon biet trang thai HIEN TAI ngay, khong phai cho den
     * nhip cap nhat sau. Day la mieng ghep lam cho hoi sinh tron ven. */
    if (has_retained && enqueue_to_subscriber(&sub_copy, &retained_copy))
        s_bus_stats.delivered++;

    return id;
}

ipc_sub_id_t ipc_bus_subscribe(uint32_t topic, ipc_handler_t *handler,
                               uint32_t deliver_as_what)
{
    if (!handler) return IPC_SUB_NONE;
    return subscribe_common(topic, handler, NULL, deliver_as_what);
}

ipc_sub_id_t ipc_bus_subscribe_service(uint32_t topic, const char *service,
                                       uint32_t deliver_as_what)
{
    if (!service) return IPC_SUB_NONE;
    return subscribe_common(topic, NULL, service, deliver_as_what);
}

/* Huy mot dang ky theo id. Id khong con hop le thi khong lam gi. */
void ipc_bus_unsubscribe(ipc_sub_id_t id)
{
    if (!s_bus_lock) return;
    if (!ipc_mutex_lock(s_bus_lock, BUS_LOCK_TIMEOUT_MS)) return;

    subscription_t *sub = subscription_from_id(id);
    if (sub) sub->in_use = false;   /* generation giu nguyen den lan cap lai */

    ipc_mutex_unlock(s_bus_lock);
}

/* Huy moi dang ky tro toi mot handler cu the. Tra ve so ban da huy. */
uint32_t ipc_bus_unsubscribe_handler(ipc_handler_t *handler)
{
    if (!handler || !s_bus_lock) return 0;
    if (!ipc_mutex_lock(s_bus_lock, BUS_LOCK_TIMEOUT_MS)) return 0;

    uint32_t removed = 0;
    for (int i = 0; i < IPC_MAX_SUBSCRIPTIONS; ++i) {
        subscription_t *sub = &s_subscriptions[i];
        if (sub->in_use && !sub->resolve_by_name && sub->handler == handler) {
            sub->in_use = false;
            removed++;
        }
    }
    ipc_mutex_unlock(s_bus_lock);
    return removed;
}

/*
 * Huy moi dang ky mang ten mot dich vu.
 *
 * Khung khoi dong dich vu goi ham nay TRUOC moi lan on_subscribe(), ke ca
 * lan dau. Neu bo buoc do, mot dich vu sau khi hoi sinh se co hai ban dang
 * ky cho cung mot chu de va nhan moi su kien HAI lan.
 */
uint32_t ipc_bus_unsubscribe_service(const char *service)
{
    if (!service || !s_bus_lock) return 0;
    if (!ipc_mutex_lock(s_bus_lock, BUS_LOCK_TIMEOUT_MS)) return 0;

    uint32_t removed = 0;
    for (int i = 0; i < IPC_MAX_SUBSCRIPTIONS; ++i) {
        subscription_t *sub = &s_subscriptions[i];
        if (sub->in_use && sub->resolve_by_name &&
            strncmp(sub->service_name, service, IPC_BUS_NAME_LEN) == 0) {
            sub->in_use = false;
            removed++;
        }
    }
    ipc_mutex_unlock(s_bus_lock);
    return removed;
}

/* ================================================================== */
/* Cong bo su kien                                                    */
/* ================================================================== */

/*
 * Cong bo mot su kien. Day la ham trung tam cua bus.
 *
 * Tra ve SO nguoi nghe da nhan duoc. Tra 0 KHONG phai loi - no thuong chi
 * co nghia la chua ai quan tam chu de nay.
 *
 * Do phuc tap:
 *   giai doan 1: O(IPC_MAX_SUBSCRIPTIONS) - quet toan bang du chi mot khop
 *   giai doan 3: O(so nguoi nghe) x (tra ten + lay pool + chen hang doi)
 *
 * Chay tren task cua nguoi cong bo. Khong bao gio chay code cua nguoi nghe.
 */
uint32_t ipc_bus_publish_ev(const ipc_event_t *event)
{
    if (!event || !s_bus_lock) return 0;
    if (!ipc_mutex_lock(s_bus_lock, BUS_LOCK_TIMEOUT_MS)) return 0;

    /*
     * Giai doan 1: chup danh sach nguoi nghe ra ban sao tren stack.
     *
     * Chup ca struct chu khong giu con tro, de sau khi tha khoa thi mot
     * lan unsubscribe dong thoi khong the lam ta doc phai slot da bi cap
     * lai cho nguoi khac.
     */
    subscription_t listeners[IPC_MAX_SUBSCRIPTIONS];
    uint32_t listener_count = 0;

    for (int i = 0; i < IPC_MAX_SUBSCRIPTIONS; ++i) {
        if (s_subscriptions[i].in_use && s_subscriptions[i].topic == event->topic)
            listeners[listener_count++] = s_subscriptions[i];
    }
    s_bus_stats.published++;

    /* Giai doan 2: tha khoa bus TRUOC khi cham vao khoa cua looper khac. */
    ipc_mutex_unlock(s_bus_lock);

    /* Giai doan 3: moi nguoi nghe mot message rieng. */
    uint32_t delivered = 0;
    for (uint32_t i = 0; i < listener_count; ++i) {
        if (enqueue_to_subscriber(&listeners[i], event)) delivered++;
        else s_bus_stats.dropped++;
    }
    s_bus_stats.delivered += delivered;

    /* Giai doan 4 xay ra sau, tren task cua tung nguoi nghe. */
    return delivered;
}

uint32_t ipc_bus_publish(uint32_t topic, int32_t arg1, int32_t arg2)
{
    ipc_event_t event = { .topic = topic, .arg1 = arg1, .arg2 = arg2,
                          .payload = NULL, .payload_len = 0 };
    return ipc_bus_publish_ev(&event);
}

/*
 * Cong bo va ghi nho gia tri cuoi cung cua chu de nay.
 *
 * Ai dang ky sau se nhan duoc no ngay trong subscribe_common(). Dung cho
 * TRANG THAI, khong dung cho du lieu do dac.
 */
uint32_t ipc_bus_publish_retained(uint32_t topic, int32_t arg1, int32_t arg2)
{
    ipc_bus_init();

    ipc_event_t event = { .topic = topic, .arg1 = arg1, .arg2 = arg2,
                          .payload = NULL, .payload_len = 0 };

    if (ipc_mutex_lock(s_bus_lock, BUS_LOCK_TIMEOUT_MS)) {
        /* Uu tien o dang co cua chu de nay; het thi lay slot trong dau tien. */
        retained_event_t *slot = retained_find(topic);
        if (!slot) {
            for (int i = 0; i < IPC_MAX_RETAINED_TOPICS; ++i) {
                if (!s_retained_events[i].in_use) { slot = &s_retained_events[i]; break; }
            }
        }
        if (slot) {
            slot->topic = topic;
            slot->last_event = event;
            slot->in_use = true;
        }
        /* Het slot giu lai: van cong bo binh thuong, chi mat tinh nang
         * "nguoi den sau biet ngay". Khong phai ly do de bo su kien. */
        ipc_mutex_unlock(s_bus_lock);
    }

    return ipc_bus_publish_ev(&event);
}

bool ipc_bus_get_retained(uint32_t topic, ipc_event_t *out)
{
    if (!out || !s_bus_lock) return false;
    if (!ipc_mutex_lock(s_bus_lock, BUS_LOCK_TIMEOUT_MS)) return false;

    retained_event_t *slot = retained_find(topic);
    if (slot) *out = slot->last_event;

    ipc_mutex_unlock(s_bus_lock);
    return slot != NULL;
}

void ipc_bus_clear_retained(uint32_t topic)
{
    if (!s_bus_lock) return;
    if (!ipc_mutex_lock(s_bus_lock, BUS_LOCK_TIMEOUT_MS)) return;

    retained_event_t *slot = retained_find(topic);
    if (slot) slot->in_use = false;

    ipc_mutex_unlock(s_bus_lock);
}

/*
 * Cong bo tu ISR.
 *
 * Duong nay hep hon ban thuong vi trong ISR khong duoc lay mutex:
 *   - khong khoa bus  -> doc thang bang dang ky
 *   - KHONG phan giai theo ten -> ServiceManager dung mutex
 *
 * Nghia la nguoi dang ky THEO TEN se khong nhan duoc su kien phat tu ISR.
 * Muon nhan tu ISR thi phai dang ky theo con tro handler.
 */
uint32_t ipc_bus_publish_from_isr(uint32_t topic, int32_t arg1, int32_t arg2,
                                  bool *higher_prio_woken)
{
    uint32_t delivered = 0;

    for (int i = 0; i < IPC_MAX_SUBSCRIPTIONS; ++i) {
        subscription_t *sub = &s_subscriptions[i];
        if (!sub->in_use) continue;
        if (sub->topic != topic) continue;
        if (sub->resolve_by_name || !sub->handler) continue;   /* bo qua: can mutex */

        ipc_message_t *msg = ipc_message_obtain();
        if (!msg) continue;
        msg->what  = sub->deliver_as_what;
        msg->topic = topic;
        msg->arg1  = arg1;
        msg->arg2  = arg2;

        if (ipc_handler_send_from_isr(sub->handler, msg, higher_prio_woken))
            delivered++;
        else
            ipc_message_recycle(msg);
    }
    return delivered;
}

/* ================================================================== */
/* Quan sat / chan doan                                               */
/* ================================================================== */

uint32_t ipc_bus_subscriber_count(uint32_t topic)
{
    if (!s_bus_lock) return 0;
    if (!ipc_mutex_lock(s_bus_lock, BUS_LOCK_TIMEOUT_MS)) return 0;

    uint32_t count = 0;
    for (int i = 0; i < IPC_MAX_SUBSCRIPTIONS; ++i) {
        if (s_subscriptions[i].in_use && s_subscriptions[i].topic == topic) count++;
    }
    ipc_mutex_unlock(s_bus_lock);
    return count;
}

void ipc_bus_get_stats(ipc_bus_stats_t *out)
{
    if (out) *out = s_bus_stats;
}

void ipc_bus_dump(void (*print)(const char *line))
{
    if (!print || !s_bus_lock) return;
    if (!ipc_mutex_lock(s_bus_lock, BUS_LOCK_TIMEOUT_MS)) return;

    char line[128];
    for (int i = 0; i < IPC_MAX_SUBSCRIPTIONS; ++i) {
        subscription_t *sub = &s_subscriptions[i];
        if (!sub->in_use) continue;

        const char *target = sub->resolve_by_name
                                 ? sub->service_name
                                 : (sub->handler && sub->handler->name
                                        ? sub->handler->name : "?");
        snprintf(line, sizeof(line), "sub[%2d] topic=%u -> %s what=%u", i,
                 (unsigned)sub->topic, target, (unsigned)sub->deliver_as_what);
        print(line);
    }

    snprintf(line, sizeof(line), "bus: pub=%u delivered=%u dropped=%u",
             (unsigned)s_bus_stats.published, (unsigned)s_bus_stats.delivered,
             (unsigned)s_bus_stats.dropped);
    print(line);

    ipc_mutex_unlock(s_bus_lock);
}
