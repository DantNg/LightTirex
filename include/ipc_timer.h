/*
 * ipc_timer.h - dich vu timer phan mem.
 *
 * Trach nhiem DUY NHAT (SRP): biet "den luc nao thi phat mot su kien".
 * No KHONG chay logic nghiep vu. Khi den han, no giao viec sang looper dich
 * duoi dang message. Nho vay:
 *   - task timer khong bao gio bi mot callback nang lam nghen,
 *   - khong co priority inversion giua cac timer,
 *   - code xu ly van chay dung tren task/context ma no thuoc ve.
 *
 * Ba kieu giao viec (OCP: them kieu moi khong phai sua nguoi dung cu):
 *   HANDLER  - post message toi mot handler cu the.
 *   SERVICE  - tra cuu handler THEO TEN tai thoi diem no. Dich vu chet roi
 *              hoi sinh van nhan duoc, vi ta phan giai lai moi lan.
 *   CALLBACK - goi thang tren task timer. Chi dung cho viec cuc ngan
 *              (bat/tat GPIO, kick watchdog). Cam block trong day.
 */
#ifndef IPC_TIMER_H
#define IPC_TIMER_H

#include "ipc_looper.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef IPC_MAX_TIMERS
#define IPC_MAX_TIMERS 24
#endif

#ifndef IPC_TIMER_NAME_LEN
#define IPC_TIMER_NAME_LEN 16
#endif

/* 0 = khong hop le. Id co ma generation ben trong nen huy nham mot timer da
 * bi thu hoi va cap phat lai cho nguoi khac la KHONG the xay ra. */
typedef uint32_t ipc_timer_id_t;
#define IPC_TIMER_NONE ((ipc_timer_id_t)0)

typedef enum {
    IPC_TIMER_ONESHOT = 0,
    IPC_TIMER_PERIODIC,
} ipc_timer_mode_t;

typedef enum {
    IPC_TIMER_TO_HANDLER = 0,
    IPC_TIMER_TO_SERVICE,
    IPC_TIMER_TO_CALLBACK,
} ipc_timer_delivery_t;

typedef struct {
    const char *name;                 /* de debug/thong ke */
    ipc_timer_mode_t     mode;
    ipc_timer_delivery_t delivery;

    uint32_t delay_ms;    /* thoi gian toi lan no dau tien */
    uint32_t period_ms;   /* chu ky (PERIODIC); 0 -> dung delay_ms */

    /* delivery = TO_HANDLER */
    ipc_handler_t *handler;
    /* delivery = TO_SERVICE */
    const char *service;              /* con tro phai song lau (string literal) */

    /* Noi dung message gui di (HANDLER/SERVICE) */
    uint32_t what;
    int32_t  arg1;
    int32_t  arg2;
    void    *payload;
    ipc_payload_free_fn payload_free; /* chi goi cho ONESHOT; PERIODIC dung payload tinh */

    /* delivery = TO_CALLBACK */
    ipc_runnable_fn callback;
    void           *callback_arg;

    /*
     * true  : tre nhieu chu ky thi chi ban 1 lan roi can gio lai tu bay gio.
     *         Chon cai nay cho heartbeat, polling, UI refresh.
     * false : bu du so lan da lo, giu dung pha (phase) ban dau.
     *         Chon cai nay cho dem thoi gian/lay mau can dung so nhip.
     */
    bool coalesce_missed;

    bool auto_start;  /* true: chay ngay khi tao */
} ipc_timer_cfg_t;

void ipc_timer_cfg_default(ipc_timer_cfg_t *cfg);

/* ---------------- vong doi engine ---------------- */

typedef struct {
    uint8_t  priority;      /* nen cao hon looper nghiep vu, thap hon supervisor */
    uint32_t stack_words;
    bool     own_task;      /* false: khong tao task, ban tu goi ipc_timer_step() */
} ipc_timer_engine_cfg_t;

void ipc_timer_engine_cfg_default(ipc_timer_engine_cfg_t *cfg);

/* An toan khi goi nhieu lan. own_task=false dung cho test tren desktop. */
bool ipc_timer_engine_start(const ipc_timer_engine_cfg_t *cfg);
void ipc_timer_engine_stop(void);

/*
 * Mot nhip khong chan: ban moi timer da den han, tra ve so timer da ban.
 * *next_delay_ms nhan khoang cho toi han ke tiep (IPC_WAIT_FOREVER neu rong).
 * Task engine goi ham nay trong vong lap; test goi truc tiep sau khi
 * ipc_fake_clock_advance().
 */
uint32_t ipc_timer_step(uint32_t *next_delay_ms);

/* ---------------- API timer ---------------- */

ipc_timer_id_t ipc_timer_create(const ipc_timer_cfg_t *cfg);

bool ipc_timer_start(ipc_timer_id_t id);
bool ipc_timer_stop(ipc_timer_id_t id);
/* Dat lai dong ho ve dau. new_delay_ms = 0 -> dung lai delay cu. */
bool ipc_timer_restart(ipc_timer_id_t id, uint32_t new_delay_ms);
/* Doi chu ky cua timer PERIODIC dang chay. */
bool ipc_timer_set_period(ipc_timer_id_t id, uint32_t period_ms);
/* Huy han: giai phong slot. Id cu tro thanh vo hieu vinh vien. */
void ipc_timer_destroy(ipc_timer_id_t id);

bool     ipc_timer_is_active(ipc_timer_id_t id);
uint32_t ipc_timer_remaining_ms(ipc_timer_id_t id); /* 0 neu khong chay */

/* Huy moi timer dang tro toi mot handler (goi khi dich vu tat). */
uint32_t ipc_timer_cancel_for_handler(ipc_handler_t *h);
uint32_t ipc_timer_cancel_for_service(const char *service);

typedef struct {
    uint32_t fired;     /* so lan da ban thanh cong */
    uint32_t missed;    /* so nhip bi lo (engine bi tre) */
    uint32_t dropped;   /* ban that bai: het message pool hoac dich chet */
    uint32_t max_lateness_ms;
} ipc_timer_stats_t;

bool ipc_timer_stats(ipc_timer_id_t id, ipc_timer_stats_t *out);
void ipc_timer_dump(void (*print)(const char *line));

/* ---------------- duong tat hay dung ---------------- */

/* Gui message tre - dung y nguyen yeu cau "sau X ms thi ban what toi handler". */
ipc_timer_id_t ipc_timer_send_delayed(ipc_handler_t *h, uint32_t what,
                                      int32_t arg1, uint32_t delay_ms);

/* Ban message dinh ky toi handler. */
ipc_timer_id_t ipc_timer_send_periodic(ipc_handler_t *h, uint32_t what,
                                       uint32_t period_ms);

/* Ban dinh ky toi dich vu theo TEN: song sot qua chu ky chet/hoi sinh. */
ipc_timer_id_t ipc_timer_send_periodic_to(const char *service, uint32_t what,
                                          uint32_t period_ms);

/* Goi callback ngan sau X ms, tren task engine. */
ipc_timer_id_t ipc_timer_call_after(ipc_runnable_fn fn, void *arg, uint32_t delay_ms);

#ifdef __cplusplus
}
#endif
#endif /* IPC_TIMER_H */
