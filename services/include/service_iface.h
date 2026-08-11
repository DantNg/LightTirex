/*
 * service_iface.h - KHUON CHUNG cho moi dich vu.
 *
 * Truoc day moi svc_*.c tu viet lay phan khoi tao: tu tao looper, tu dang ky
 * ten, tu huy dang ky bus cu, tu dat co san sang. Lap lai o nam noi thi som
 * muon co mot noi quen mot buoc - va thuc te da xay ra dung nhu vay.
 *
 * Bay gio moi dich vu chi khai bao mot bang app_service_t va dien vao cac
 * moc doi. Khung se lo phan con lai, theo dung mot thu tu, khong bao gio
 * quen buoc nao.
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │ VONG DOI (khung goi, dich vu chi dien vao)                  │
 *   ├─────────────────────────────────────────────────────────────┤
 *   │ app_service_start()                                          │
 *   │   ├─ tao looper mang ten dich vu                             │
 *   │   └─ tao task (hoac khong, neu dang chay che do test)        │
 *   │                                                              │
 *   │ [trong task moi] - va CHAY LAI moi lan dich vu hoi sinh:     │
 *   │   1. ipc_handler_init  + ipc_service_register  (khung)       │
 *   │   2. ipc_bus_unsubscribe_service(name)          (khung)      │
 *   │   3. on_create()      - dung lai trang thai, cam driver      │
 *   │   4. on_subscribe()   - dang ky nghe cac chu de              │
 *   │   5. dat co san sang tren event group           (khung)      │
 *   │                                                              │
 *   │ [moi message toi]                                            │
 *   │   on_receive(self, msg)                                      │
 *   └─────────────────────────────────────────────────────────────┘
 *
 * Buoc 2 la ly do chinh khung ton tai: neu dich vu tu dang ky nghe ma quen
 * huy dang ky cu, thi sau moi lan hoi sinh no se nhan MOI su kien HAI lan.
 * Day la loi rat kho thay khi chay that. Khung lam viec do, khong ai quen duoc.
 */
#ifndef APP_SERVICE_IFACE_H
#define APP_SERVICE_IFACE_H

#include "ipc_looper.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app_service app_service_t;

/* Ma tra ve cua get/set dung chung, de phan biet "khong co khoa do" voi
 * "gia tri that su bang 0". */
#define SVC_VALUE_INVALID INT32_MIN

struct app_service {
    /* ---- khai bao (dich vu dien luc bien dich) ---- */
    const char *name;              /* trung voi ten dang ky ServiceManager */
    uint8_t     priority;
    uint32_t    stack_words;
    uint32_t    heartbeat_timeout_ms;
    uint32_t    ready_bit;         /* co bao san sang tren event group */
    bool        purge_queue_on_restart;

    /* ---- moc doi (khung goi) ---- */

    /* Chay trong task, moi lan khoi dong VA moi lan hoi sinh.
     * Dung de dung lai trang thai va cam driver. KHONG dang ky nghe o day. */
    void (*on_create)(app_service_t *self);

    /* Dang ky nghe bus o day. Khung da huy dang ky cu truoc khi goi. */
    void (*on_subscribe)(app_service_t *self);

    /* Xu ly mot message. Tra true neu da xu ly xong. */
    bool (*on_receive)(app_service_t *self, ipc_message_t *msg);

    /* Goi khi dich vu bi dung han (health quyet dinh giet). Co the NULL. */
    void (*on_destroy)(app_service_t *self);

    /* ---- doc/ghi tham so, dung chung mot chu ky ---- */

    /* Tra SVC_VALUE_INVALID neu khong biet khoa. def dung khi chua co gia tri. */
    int32_t (*get)(app_service_t *self, uint32_t key, int32_t def);
    /* Tra false neu khoa khong hop le hoac gia tri bi tu choi. */
    bool    (*set)(app_service_t *self, uint32_t key, int32_t value);

    /* ---- khung dien luc chay ---- */
    ipc_looper_t *looper;
    ipc_handler_t handler;
    void         *state;           /* tro toi state rieng cua dich vu */
    void         *ctx;             /* cau hinh ung dung (app_cfg_t) */
};

/*
 * Tao looper cho dich vu va chay no.
 * spawn_task = false: chi tao looper, khong tao task; nguoi goi tu bom nhip
 * bang ipc_looper_poll(). Cac moc doi van chay y het.
 */
bool app_service_start(app_service_t *svc, void *ctx, bool spawn_task);

/* Khoi dong lai mot dich vu - dung duong ma supervisor se dung. */
bool app_service_restart(app_service_t *svc);

/* Tra ve dich vu theo ten, hoac NULL. */
app_service_t *app_service_find(const char *name);

/* Duong tat: goi get/set cua mot dich vu theo ten. */
int32_t app_service_get(const char *name, uint32_t key, int32_t def);
bool    app_service_set(const char *name, uint32_t key, int32_t value);

#ifdef __cplusplus
}
#endif
#endif /* APP_SERVICE_IFACE_H */
