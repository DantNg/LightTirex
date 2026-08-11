/* port_freertos.c - hien thuc ipc_port.h tren FreeRTOS (ESP-IDF / vanilla). */
#include "ipc_port.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#ifndef IPC_TLS_INDEX
#define IPC_TLS_INDEX 0
#endif

static inline TickType_t to_ticks(ipc_tick_t ms)
{
    if (ms == IPC_WAIT_FOREVER) return portMAX_DELAY;
    return pdMS_TO_TICKS(ms);
}

ipc_tick_t ipc_now_ms(void)
{
    return (ipc_tick_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void ipc_sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

ipc_mutex_t ipc_mutex_create(void) { return (ipc_mutex_t)xSemaphoreCreateRecursiveMutex(); }

void ipc_mutex_destroy(ipc_mutex_t m)
{
    if (m) vSemaphoreDelete((SemaphoreHandle_t)m);
}

bool ipc_mutex_lock(ipc_mutex_t m, ipc_tick_t timeout_ms)
{
    return xSemaphoreTakeRecursive((SemaphoreHandle_t)m, to_ticks(timeout_ms)) == pdTRUE;
}

void ipc_mutex_unlock(ipc_mutex_t m)
{
    xSemaphoreGiveRecursive((SemaphoreHandle_t)m);
}

ipc_sem_t ipc_sem_create(void) { return (ipc_sem_t)xSemaphoreCreateBinary(); }

void ipc_sem_destroy(ipc_sem_t s)
{
    if (s) vSemaphoreDelete((SemaphoreHandle_t)s);
}

bool ipc_sem_take(ipc_sem_t s, ipc_tick_t timeout_ms)
{
    return xSemaphoreTake((SemaphoreHandle_t)s, to_ticks(timeout_ms)) == pdTRUE;
}

void ipc_sem_give(ipc_sem_t s) { xSemaphoreGive((SemaphoreHandle_t)s); }

void ipc_sem_give_from_isr(ipc_sem_t s, bool *higher_prio_woken)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)s, &hp);
    if (higher_prio_woken) *higher_prio_woken = (hp == pdTRUE);
}

bool ipc_task_create(ipc_task_t *out, const char *name, ipc_task_fn_t fn,
                     void *arg, uint32_t stack_words, uint8_t prio)
{
    TaskHandle_t h = NULL;
    if (xTaskCreate(fn, name, stack_words, arg, prio, &h) != pdPASS) return false;
    if (out) *out = (ipc_task_t)h;
    return true;
}

void ipc_task_delete(ipc_task_t t) { vTaskDelete((TaskHandle_t)t); }

/*
 * Luu y: sau vTaskDelete tren task tao dong, idle task se giai phong TCB,
 * handle tro thanh dangling. Supervisor KHONG duoc dua vao rieng ham nay -
 * no dung heartbeat lam nguon su that chinh, ham nay chi la kiem tra phu.
 */
bool ipc_task_is_alive(ipc_task_t t)
{
    if (!t) return false;
    eTaskState st = eTaskGetState((TaskHandle_t)t);
    return st != eDeleted && st != eInvalid;
}

ipc_task_t ipc_task_self(void) { return (ipc_task_t)xTaskGetCurrentTaskHandle(); }

void *ipc_tls_get(void)
{
    return pvTaskGetThreadLocalStoragePointer(NULL, IPC_TLS_INDEX);
}

void ipc_tls_set(void *p)
{
    vTaskSetThreadLocalStoragePointer(NULL, IPC_TLS_INDEX, p);
}

#if defined(ESP_PLATFORM)
static portMUX_TYPE s_ipc_spin = portMUX_INITIALIZER_UNLOCKED;
#define IPC_CRIT_ENTER() portENTER_CRITICAL_SAFE(&s_ipc_spin)
#define IPC_CRIT_EXIT()  portEXIT_CRITICAL_SAFE(&s_ipc_spin)
#else
#define IPC_CRIT_ENTER() taskENTER_CRITICAL()
#define IPC_CRIT_EXIT()  taskEXIT_CRITICAL()
#endif

void ipc_enter_critical(void)
{
#if !defined(ESP_PLATFORM)
    if (ipc_in_isr()) return; /* caller dung *_from_isr thay the */
#endif
    IPC_CRIT_ENTER();
}

void ipc_exit_critical(void)
{
#if !defined(ESP_PLATFORM)
    if (ipc_in_isr()) return;
#endif
    IPC_CRIT_EXIT();
}

bool ipc_in_isr(void)
{
#if defined(ESP_PLATFORM)
    return xPortInIsrContext();
#else
    return xPortIsInsideInterrupt() == pdTRUE;
#endif
}
