/*
 * ipc_event.h - event bus: mo hinh observer/listener giua cac dich vu.
 *
 * Vi sao can, khi da co ipc_service_send()?
 *   send() la giao tiep DIEM-DEN-DIEM: ben gui phai biet ten ben nhan. Them
 *   mot nguoi nghe moi thi phai sua code ben gui. Bus dao nguoc phu thuoc:
 *   ben gui chi cong bo "co mau moi", ai quan tam thi tu dang ky. Them nguoi
 *   nghe khong dong vao ben gui mot dong nao (OCP).
 *
 * Quan he voi looper: bus KHONG chay callback cua nguoi nghe. No chuyen su
 * kien thanh message roi day vao hang doi cua looper tuong ung. Nghia la
 * nguoi cong bo khong bao gio bi chan boi nguoi nghe cham.
 *
 * QUYEN SO HUU PAYLOAD: bus khong bao gio giai phong payload. Du lieu nho
 * thi nhet vao arg1/arg2; du lieu lon thi tro toi bo nho song lau (static/
 * retained) do ben cong bo giu. Mot su kien co the toi nhieu nguoi nghe nen
 * "ai free" se khong bao gio ro rang - vi vay bus tu choi trach nhiem do.
 */
#ifndef IPC_EVENT_H
#define IPC_EVENT_H

#include "ipc_looper.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef IPC_MAX_SUBSCRIPTIONS
#define IPC_MAX_SUBSCRIPTIONS 32
#endif

#ifndef IPC_MAX_RETAINED_TOPICS
#define IPC_MAX_RETAINED_TOPICS 12
#endif

#ifndef IPC_BUS_NAME_LEN
#define IPC_BUS_NAME_LEN 16
#endif

typedef uint32_t ipc_sub_id_t;
#define IPC_SUB_NONE ((ipc_sub_id_t)0)

typedef struct {
    uint32_t topic;
    int32_t  arg1;
    int32_t  arg2;
    void    *payload;      /* do ben cong bo so huu, bus khong free */
    uint32_t payload_len;
} ipc_event_t;

void ipc_bus_init(void);

/*
 * Dang ky nghe bang con tro handler. Khi co su kien, nguoi nghe nhan mot
 * message voi msg->what = what va msg->topic = topic.
 */
ipc_sub_id_t ipc_bus_subscribe(uint32_t topic, ipc_handler_t *h, uint32_t what);

/*
 * Dang ky nghe bang TEN dich vu. Bus phan giai handler tai thoi diem cong bo
 * nen dich vu chet roi hoi sinh (handler moi, con tro moi) van tiep tuc nhan
 * su kien. Day la cach nen dung cho dich vu co the bi khoi dong lai.
 */
ipc_sub_id_t ipc_bus_subscribe_service(uint32_t topic, const char *service,
                                       uint32_t what);

void ipc_bus_unsubscribe(ipc_sub_id_t id);
uint32_t ipc_bus_unsubscribe_handler(ipc_handler_t *h);
uint32_t ipc_bus_unsubscribe_service(const char *service);

/* Tra ve so nguoi nghe da nhan duoc. 0 nghia la khong ai nghe (khong phai loi). */
uint32_t ipc_bus_publish(uint32_t topic, int32_t arg1, int32_t arg2);
uint32_t ipc_bus_publish_ev(const ipc_event_t *ev);

/*
 * Cong bo va GIU LAI gia tri cuoi cung. Ai dang ky sau se nhan ngay gia tri
 * do ma khong phai cho su kien ke tiep.
 *
 * Dung cho trang thai (cau hinh hien tai, tinh trang mang, trang thai suc
 * khoe). Nho no ma mot dich vu vua hoi sinh lay lai duoc buc tranh hien tai
 * ngay lap tuc, thay vi chay mu cho den nhip cap nhat sau.
 */
uint32_t ipc_bus_publish_retained(uint32_t topic, int32_t arg1, int32_t arg2);
bool     ipc_bus_get_retained(uint32_t topic, ipc_event_t *out);
void     ipc_bus_clear_retained(uint32_t topic);

/* Cong bo tu ISR: khong lay mutex, khong phan giai theo ten. */
uint32_t ipc_bus_publish_from_isr(uint32_t topic, int32_t arg1, int32_t arg2,
                                  bool *higher_prio_woken);

uint32_t ipc_bus_subscriber_count(uint32_t topic);
void     ipc_bus_dump(void (*print)(const char *line));

typedef struct {
    uint32_t published;
    uint32_t delivered;
    uint32_t dropped;      /* het message pool hoac dich vu khong ton tai */
} ipc_bus_stats_t;

void ipc_bus_get_stats(ipc_bus_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif /* IPC_EVENT_H */
