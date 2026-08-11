/*
 * app.h - lap rap va dieu khien ca he thong.
 */
#ifndef APP_H
#define APP_H

#include "drivers/drivers.h"
#include "ipc_config.h"
#include "ipc_event_group.h"
#include "ipc_looper.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /*
     * true  : moi dich vu chay tren mot task RTOS that (tren board).
     * false : chi tao looper, khong tao task. Nguoi goi tu bom nhip bang
     *         app_poll_all(). Day la che do test tren desktop: don luong,
     *         thoi gian ao, khong bao gio flaky. Cac moc doi cua dich vu
     *         chay y het hai che do.
     */
    bool spawn_tasks;

    sensor_driver_t   *sensor;      /* NULL -> ban mo phong */
    cloud_client_t    *cloud;       /* NULL -> server gia noi bo */
    ipc_cfg_storage_t *cfg_storage; /* NULL -> file "app.cfg" */
} app_cfg_t;

void app_cfg_default(app_cfg_t *cfg);

bool app_start(const app_cfg_t *cfg);
void app_stop(void);

/*
 * Chay moi looper cho den khi khong con message den han. Tra ve tong so
 * message da xu ly. CHI dung khi spawn_tasks = false.
 */
uint32_t app_poll_all(void);

ipc_looper_t *app_looper(const char *service_name);
void app_dump(void (*print)(const char *line));

#ifdef __cplusplus
}
#endif
#endif /* APP_H */
