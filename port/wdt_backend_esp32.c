/*
 * wdt_backend_esp32.c - backend watchdog phan cung cho ESP-IDF.
 *
 * Chi la mot ban cai dat cua ipc_wdt_backend_t. Core watchdog khong biet
 * file nay ton tai (OCP): them STM32 IWDG hay nRF WDT chi la them mot file
 * tuong tu, khong sua mot dong nao trong ipc_watchdog.c.
 */
#include "ipc_watchdog.h"

#ifdef ESP_PLATFORM

#include "esp_task_wdt.h"
#include "esp_system.h"

static bool s_subscribed;

static bool esp_wdt_init(ipc_wdt_backend_t *self, uint32_t timeout_ms)
{
    (void)self;
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t cfg = {
        .timeout_ms = timeout_ms,
        .idle_core_mask = 0,     /* khong giam sat idle task: ta tu giam sat */
        .trigger_panic = true,   /* panic -> co backtrace, tot hon reset im lang */
    };
    esp_err_t err = esp_task_wdt_init(&cfg);
    if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_reconfigure(&cfg);
    return err == ESP_OK;
#else
    return esp_task_wdt_init(timeout_ms / 1000, true) == ESP_OK;
#endif
}

static void esp_wdt_feed(ipc_wdt_backend_t *self)
{
    (void)self;
    /* Dang ky tre: feed() luon chay tren task watchdog, con init() thi khong
     * chac. Phai subscribe dung task se cho an. */
    if (!s_subscribed) {
        if (esp_task_wdt_add(NULL) != ESP_OK) return;
        s_subscribed = true;
    }
    esp_task_wdt_reset();
}

static void esp_wdt_reset_system(ipc_wdt_backend_t *self)
{
    (void)self;
    esp_restart();   /* khong tro ve */
}

static ipc_wdt_backend_t s_esp_backend = {
    .name = "esp32",
    .init = esp_wdt_init,
    .feed = esp_wdt_feed,
    .reset_system = esp_wdt_reset_system,
    .impl = 0,
};

ipc_wdt_backend_t *ipc_wdt_backend_esp32(void) { return &s_esp_backend; }

#endif /* ESP_PLATFORM */
