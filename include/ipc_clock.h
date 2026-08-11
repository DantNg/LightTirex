/*
 * ipc_clock.h - nguon thoi gian truu tuong.
 *
 * DIP (Dependency Inversion): looper, timer, watchdog KHONG goi thang
 * ipc_now_ms() cua RTOS nua, ma goi qua interface nay. Nho vay tren desktop
 * ta cam duoc kim dong ho: test "sau 30 phut thi sao" chay het trong 1 us,
 * deterministic, khong sleep, khong flaky.
 *
 * ISP: interface chi co dung 1 phuong thuc. Ai can gio thi chi phu thuoc
 * vao 1 ham, khong keo theo ca RTOS.
 */
#ifndef IPC_CLOCK_H
#define IPC_CLOCK_H

#include "ipc_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ipc_clock ipc_clock_t;

struct ipc_clock {
    ipc_tick_t (*now_ms)(const ipc_clock_t *self);
    void       *impl;   /* du lieu rieng cua ban cai dat */
    const char *name;   /* de debug: "system" / "fake" */
};

/* Dong ho that, lay tu RTOS/OS. Luon co san. */
const ipc_clock_t *ipc_clock_system(void);

/* Dong ho dang duoc toan he thong dung. Mac dinh la system clock. */
const ipc_clock_t *ipc_clock_get(void);

/*
 * Cam kim dong ho. CHI goi trong test, truoc khi tao looper/timer.
 * Truyen NULL de tra ve dong ho that.
 */
void ipc_clock_set(const ipc_clock_t *clk);

/* Duong tat: gio hien tai theo dong ho dang dung. */
static inline ipc_tick_t ipc_clock_now(void)
{
    const ipc_clock_t *c = ipc_clock_get();
    return c->now_ms(c);
}

/* ---------------- Fake clock (chi dung cho test) ---------------- */

typedef struct {
    ipc_clock_t  base;      /* phai nam dau: LSP - dung duoc o moi cho can ipc_clock_t */
    volatile ipc_tick_t now;
} ipc_fake_clock_t;

void ipc_fake_clock_init(ipc_fake_clock_t *fc, ipc_tick_t start_ms);
void ipc_fake_clock_advance(ipc_fake_clock_t *fc, uint32_t ms);
void ipc_fake_clock_set(ipc_fake_clock_t *fc, ipc_tick_t ms);

#ifdef __cplusplus
}
#endif
#endif /* IPC_CLOCK_H */
