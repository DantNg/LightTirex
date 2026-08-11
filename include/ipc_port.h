/*
 * ipc_port.h - RTOS abstraction layer.
 *
 * Toan bo framework chi phu thuoc vao file nay. Muon doi RTOS (Zephyr,
 * ThreadX, CMSIS-RTOS2) chi can viet lai mot file port_xxx.c.
 */
#ifndef IPC_PORT_H
#define IPC_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t ipc_tick_t;
#define IPC_WAIT_FOREVER ((ipc_tick_t)0xFFFFFFFFu)

typedef void *ipc_mutex_t;
typedef void *ipc_sem_t;
typedef void *ipc_task_t;

typedef void (*ipc_task_fn_t)(void *arg);

/* --- time --- */
ipc_tick_t ipc_now_ms(void);
void       ipc_sleep_ms(uint32_t ms);

/* --- mutex (recursive, priority-inheriting neu RTOS ho tro) --- */
ipc_mutex_t ipc_mutex_create(void);
void        ipc_mutex_destroy(ipc_mutex_t m);
bool        ipc_mutex_lock(ipc_mutex_t m, ipc_tick_t timeout_ms);
void        ipc_mutex_unlock(ipc_mutex_t m);

/* --- binary semaphore: tin hieu "co viec moi trong queue" --- */
ipc_sem_t ipc_sem_create(void);
void      ipc_sem_destroy(ipc_sem_t s);
/* Tra ve false neu timeout. */
bool      ipc_sem_take(ipc_sem_t s, ipc_tick_t timeout_ms);
void      ipc_sem_give(ipc_sem_t s);
void      ipc_sem_give_from_isr(ipc_sem_t s, bool *higher_prio_woken);

/* --- task --- */
bool ipc_task_create(ipc_task_t *out, const char *name, ipc_task_fn_t fn,
                     void *arg, uint32_t stack_words, uint8_t prio);
void ipc_task_delete(ipc_task_t t);
bool ipc_task_is_alive(ipc_task_t t);
ipc_task_t ipc_task_self(void);

/* --- thread local storage: dung de map task -> looper hien hanh --- */
void *ipc_tls_get(void);
void  ipc_tls_set(void *p);

/* --- critical section ngan (dung cho enqueue tu ISR) --- */
void ipc_enter_critical(void);
void ipc_exit_critical(void);

bool ipc_in_isr(void);

#ifdef __cplusplus
}
#endif
#endif /* IPC_PORT_H */
