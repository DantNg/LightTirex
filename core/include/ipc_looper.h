/*
 * ipc_looper.h - Looper + MessageQueue cho RTOS.
 *
 * Anh xa khai niem:
 *   Linux/Android            RTOS (o day)
 *   -------------            ------------
 *   process                  task (thread)
 *   Looper.loop()            ipc_looper_run() chay trong than task
 *   MessageQueue             danh sach lien ket sorted theo when_ms
 *   ThreadLocal<Looper>      ipc_tls_get/set
 *   Handler                  ipc_handler_t (callback + looper dich)
 *   Binder/ServiceManager    ipc_service_* (tra cuu handler theo ten)
 *   init/zygote respawn      ipc_supervisor (hoi sinh task chet)
 *
 * DIEM MAU CHOT VE CHIU LOI:
 *   ipc_looper_t la object doc lap voi task. Task chi la "co bap" chay vong
 *   lap. Task chet -> object van con nguyen -> supervisor tao task moi gan
 *   lai vao dung object do, message dang xep hang KHONG mat.
 */
#ifndef IPC_LOOPER_H
#define IPC_LOOPER_H

#include "ipc_message.h"
#include "ipc_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ipc_looper ipc_looper_t;
typedef struct ipc_handler ipc_handler_t;

typedef enum {
    IPC_LOOPER_IDLE = 0,   /* da tao, chua chay */
    IPC_LOOPER_RUNNING,
    IPC_LOOPER_QUITTING,
    IPC_LOOPER_DEAD,       /* task bien mat / treo, cho supervisor */
    IPC_LOOPER_STOPPED,    /* dung han theo yeu cau, khong hoi sinh */
} ipc_looper_state_t;

/* Ly do looper bi coi la chet. */
typedef enum {
    IPC_DEATH_NONE = 0,
    IPC_DEATH_TASK_GONE,     /* task tu thoat / bi xoa */
    IPC_DEATH_HEARTBEAT,     /* qua han heartbeat: treo hoac deadlock */
    IPC_DEATH_MANUAL,        /* nguoi dung ep restart */
} ipc_death_reason_t;

typedef void (*ipc_looper_hook_fn)(ipc_looper_t *lp, void *user);
typedef void (*ipc_looper_death_fn)(ipc_looper_t *lp, ipc_death_reason_t why,
                                    uint32_t generation, void *user);

typedef struct {
    const char *name;            /* ten task + ten dich vu */
    uint32_t    stack_words;     /* stack cho task */
    uint8_t     priority;
    /* Ngan hon = phat hien treo nhanh hon, nhung phai lon hon thoi gian xu ly
     * message lau nhat. 0 = tat giam sat heartbeat. */
    uint32_t    heartbeat_timeout_ms;
    uint32_t    max_restarts;    /* trong cua so restart_window_ms; 0 = khong gioi han */
    uint32_t    restart_window_ms;
    uint32_t    restart_backoff_ms;
    bool        purge_queue_on_restart; /* true: bo message cu khi hoi sinh */

    ipc_looper_hook_fn  on_start;   /* chay trong task moi, truoc vong lap */
    ipc_looper_hook_fn  on_stop;    /* chay khi thoat vong lap binh thuong */
    ipc_looper_death_fn on_death;   /* chay trong task supervisor */
    void *user;
} ipc_looper_cfg_t;

/* Gia tri mac dinh hop ly cho mot dich vu thong thuong. */
void ipc_looper_cfg_default(ipc_looper_cfg_t *cfg, const char *name);

/* Tao object looper (chua tao task). Tra NULL neu het slot. */
ipc_looper_t *ipc_looper_create(const ipc_looper_cfg_t *cfg);

/* Tra slot ve. Chi hop le khi looper KHONG con chay (quit truoc do). */
void ipc_looper_destroy(ipc_looper_t *lp);

/* Tao task va bat dau chay vong lap. */
bool ipc_looper_start(ipc_looper_t *lp);

/* Chay vong lap ngay tren task hien tai (kieu Looper.prepare + loop()). */
void ipc_looper_run(ipc_looper_t *lp);

/*
 * Chay MOT nhip khong chan: xu ly moi message da den han roi tra ve so
 * message da xu ly. Day la cua ngo test tren desktop - ket hop voi
 * ipc_fake_clock, toan bo he thong chay don luong, deterministic, khong sleep.
 * max_msgs = 0 nghia la khong gioi han.
 */
uint32_t ipc_looper_poll(ipc_looper_t *lp, uint32_t max_msgs);

/* true neu looper co task RTOS rieng; false neu dang duoc bom nhip bang tay. */
bool ipc_looper_is_task_driven(const ipc_looper_t *lp);

/*
 * Khoi dong lai looper MA KHONG tao task moi: tang generation, giu nguyen
 * hang doi, chay lai on_start. Dung khi nguoi goi tu bom nhip (test), noi
 * ma tao them mot task that se pha vo tinh don luong. Tren board hay dung
 * ipc_looper_revive() qua supervisor.
 */
bool ipc_looper_restart_inplace(ipc_looper_t *lp);

/* Looper gan voi task dang chay, hoac NULL. */
ipc_looper_t *ipc_looper_my_looper(void);

/* Xin thoat. safely=true: xu ly het message da den han roi moi thoat. */
void ipc_looper_quit(ipc_looper_t *lp, bool safely);

/* Dung han + khong hoi sinh nua (supervisor bo qua). */
void ipc_looper_stop_permanently(ipc_looper_t *lp);

const char        *ipc_looper_name(const ipc_looper_t *lp);
ipc_looper_state_t ipc_looper_state(const ipc_looper_t *lp);
uint32_t           ipc_looper_generation(const ipc_looper_t *lp); /* tang moi lan hoi sinh */
uint32_t           ipc_looper_pending(const ipc_looper_t *lp);
uint32_t           ipc_looper_restart_count(const ipc_looper_t *lp);
/* Bao lau roi looper chua cap nhat nhip tim. Watchdog dung so nay thay cho
 * viec bat looper phai tu kick. */
uint32_t           ipc_looper_since_heartbeat_ms(const ipc_looper_t *lp);

/* --- danh cho supervisor --- */
bool ipc_looper_check_alive(ipc_looper_t *lp, ipc_death_reason_t *why);
bool ipc_looper_revive(ipc_looper_t *lp, ipc_death_reason_t why);
uint32_t      ipc_looper_count(void);
ipc_looper_t *ipc_looper_at(uint32_t index);
const ipc_looper_cfg_t *ipc_looper_cfg(const ipc_looper_t *lp);

/* ---------------- Handler ---------------- */

/* Tra ve true neu da xu ly xong message. */
typedef bool (*ipc_handler_cb)(ipc_handler_t *h, ipc_message_t *msg, void *user);

struct ipc_handler {
    ipc_looper_t  *looper;
    ipc_handler_cb cb;
    void          *user;
    const char    *name;
};

void ipc_handler_init(ipc_handler_t *h, ipc_looper_t *lp, ipc_handler_cb cb,
                      void *user, const char *name);

/* Gui message. Sau khi goi, quyen so huu msg thuoc ve queue.
 * Tra false neu looper dang STOPPED/QUITTING (msg duoc recycle tu dong). */
bool ipc_handler_send(ipc_handler_t *h, ipc_message_t *msg);
bool ipc_handler_send_delayed(ipc_handler_t *h, ipc_message_t *msg, uint32_t delay_ms);
bool ipc_handler_send_at_front(ipc_handler_t *h, ipc_message_t *msg);
bool ipc_handler_send_empty(ipc_handler_t *h, uint32_t what);
bool ipc_handler_send_empty_delayed(ipc_handler_t *h, uint32_t what, uint32_t delay_ms);
bool ipc_handler_post(ipc_handler_t *h, ipc_runnable_fn fn, void *arg);
bool ipc_handler_post_delayed(ipc_handler_t *h, ipc_runnable_fn fn, void *arg, uint32_t delay_ms);

/* Ban tu ISR: khong cap phat, nguoi goi tu obtain truoc (hoac dung pool). */
bool ipc_handler_send_from_isr(ipc_handler_t *h, ipc_message_t *msg,
                               bool *higher_prio_woken);

/* Huy message chua dispatch. what == IPC_WHAT_ANY -> huy tat ca cua handler. */
#define IPC_WHAT_ANY 0xFFFFFFFFu
uint32_t ipc_handler_remove(ipc_handler_t *h, uint32_t what);
bool     ipc_handler_has(ipc_handler_t *h, uint32_t what);

/* Goi dong bo: cho den khi message duoc xu ly xong (co timeout).
 * KHONG duoc goi tu chinh looper dich -> se tra false ngay (tranh deadlock). */
bool ipc_handler_send_sync(ipc_handler_t *h, uint32_t what, int32_t arg1,
                           void *payload, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif /* IPC_LOOPER_H */
