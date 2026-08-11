/*
 * ipc_message.h - Message + message pool.
 *
 * Tuong duong android.os.Message. Khong dung malloc trong duong chay nong:
 * tat ca message lay tu mot pool tinh -> khong phan manh heap, deterministic,
 * va co the thu hoi hang loat khi mot task chet.
 */
#ifndef IPC_MESSAGE_H
#define IPC_MESSAGE_H

#include "ipc_port.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_handler;
typedef struct ipc_message ipc_message_t;

/* Runnable: post mot callback thay vi mot "what" code (Handler.post). */
typedef void (*ipc_runnable_fn)(void *arg);

/* Ham giai phong payload do nguoi dung cap phat (vd heap buffer). */
typedef void (*ipc_payload_free_fn)(void *payload);

struct ipc_message {
    /* --- phan nguoi dung --- */
    uint32_t what;      /* ma lenh */
    int32_t  arg1;
    int32_t  arg2;
    void    *payload;   /* con tro du lieu kem theo */
    uint32_t payload_len;
    ipc_payload_free_fn payload_free; /* NULL neu payload la static/khong can free */

    ipc_runnable_fn runnable;  /* neu != NULL: dispatch goi runnable(payload) */

    /* --- phan noi bo --- */
    struct ipc_handler *target;
    ipc_tick_t     when_ms;    /* moc thoi gian duoc phep dispatch */
    uint8_t        prio;       /* 0 = cao nhat, dung cho barrier/async */
    uint8_t        in_use;
    uint16_t       pool_index; /* IPC_MSG_NOT_POOLED neu do nguoi dung cap */
    ipc_message_t *next;       /* lien ket trong queue (sorted theo when_ms) */
    uint32_t       owner_tag;  /* looper id so huu -> dung khi thu hoi luc crash */
    uint32_t       seq;        /* thu tu FIFO khi cung when_ms */
    void          *sync_token; /* != NULL neu day la loi goi dong bo */
};

#define IPC_MSG_NOT_POOLED 0xFFFFu

/* Khoi tao pool. Goi mot lan luc boot, truoc moi API khac. */
void ipc_message_pool_init(void);

/* Lay message rong. Tra NULL neu pool can. An toan goi tu ISR. */
ipc_message_t *ipc_message_obtain(void);

/* Tra message ve pool + goi payload_free neu co. */
void ipc_message_recycle(ipc_message_t *m);

/* Thu hoi moi message dang mang owner_tag (dung khi looper bi reset). */
uint32_t ipc_message_reclaim_by_owner(uint32_t owner_tag);

/* Thong ke de giam sat ro ri. */
void ipc_message_pool_stats(uint32_t *free_count, uint32_t *low_watermark);

#ifdef __cplusplus
}
#endif
#endif /* IPC_MESSAGE_H */
