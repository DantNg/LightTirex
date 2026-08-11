/*
 * ipc_event_group.h - nhom co su kien (event group), kieu FreeRTOS nhung
 * viet tren lop port nen chay duoc ca tren desktop.
 *
 * Ba cach dong bo trong he nay, dung dung cho:
 *
 *   Bus (ipc_event.h)   - "co chuyen gi xay ra": nhieu nguoi nghe, khong ai
 *                         bi chan. Dung cho luong du lieu.
 *   Semaphore give/take - "co dung MOT viec can lam": danh thuc mot nguoi
 *                         cho. Chinh la thu looper dung ben trong.
 *   Event group         - "cac dieu kien da du chua": cho NHIEU dieu kien
 *                         cung luc (AND/OR). Dung cho trinh tu khoi dong va
 *                         cho cac trang thai keo dai (mang len, cau hinh
 *                         da nap, cam bien da san sang).
 *
 * Ly do can event group ma semaphore khong thay duoc: "doi cho den khi
 * CA config VA network deu san sang" bang semaphore se phai dem thu cong va
 * de sai; bang event group la mot loi goi.
 */
#ifndef IPC_EVENT_GROUP_H
#define IPC_EVENT_GROUP_H

#include "ipc_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef IPC_EG_MAX_WAITERS
#define IPC_EG_MAX_WAITERS 8
#endif

typedef struct ipc_event_group ipc_event_group_t;

ipc_event_group_t *ipc_event_group_create(void);
void ipc_event_group_destroy(ipc_event_group_t *g);

/* Bat co. Tra ve gia tri co sau khi bat. Danh thuc moi nguoi cho da du dieu kien. */
uint32_t ipc_event_group_set(ipc_event_group_t *g, uint32_t bits);

/* Xoa co. Tra ve gia tri co sau khi xoa. */
uint32_t ipc_event_group_clear(ipc_event_group_t *g, uint32_t bits);

uint32_t ipc_event_group_get(const ipc_event_group_t *g);

/*
 * Cho dieu kien.
 *   wait_all = true  : cho DU tat ca bit trong `bits` (AND)
 *   wait_all = false : cho BAT KY bit nao (OR)
 *   clear_on_exit    : xoa cac bit da thoa man truoc khi tra ve
 *   timeout_ms       : 0 = kiem tra roi ve ngay (khong chan)
 *
 * Tra ve gia tri co tai thoi diem thoa man, hoac 0 neu het gio.
 * Luu y: dung timeout that (khong phai fake clock) vi no chan tren semaphore
 * cua RTOS. Test don luong nen dung timeout 0.
 */
uint32_t ipc_event_group_wait(ipc_event_group_t *g, uint32_t bits, bool wait_all,
                              bool clear_on_exit, uint32_t timeout_ms);

/* Bat co tu ISR. */
void ipc_event_group_set_from_isr(ipc_event_group_t *g, uint32_t bits,
                                  bool *higher_prio_woken);

#ifdef __cplusplus
}
#endif
#endif /* IPC_EVENT_GROUP_H */
