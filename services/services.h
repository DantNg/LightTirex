/*
 * services.h - API khoi dong cua tung dich vu + cua ca ung dung.
 *
 * Moi dich vu deu theo cung mot khuon:
 *   - svc_x_start(cfg) tao looper rieng cho no,
 *   - toan bo viec khoi tao nam trong on_start CHU KHONG o svc_x_start,
 *     vi on_start se chay lai moi lan dich vu duoc hoi sinh sau khi chet,
 *   - dich vu tu dang ky ten voi ServiceManager va tu dang ky nghe tren bus.
 *
 * Cac ham *_peek_* la cua so quan sat danh cho test va cho lenh chan doan;
 * chung chi doc, khong dieu khien gi.
 */
#ifndef APP_SERVICES_H
#define APP_SERVICES_H

#include "app_events.h"
#include "drivers.h"
#include "ipc_config.h"
#include "ipc_event_group.h"
#include "ipc_looper.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /*
     * true  : moi dich vu chay tren task RTOS that (tren board).
     * false : chi tao looper, khong tao task. Nguoi goi tu bom nhip bang
     *         app_poll_all(). Day la che do test tren desktop: don luong,
     *         thoi gian ao, khong bao gio flaky.
     */
    bool spawn_tasks;

    sensor_driver_t   *sensor;      /* NULL -> ban mo phong */
    cloud_client_t    *cloud;       /* NULL -> server gia noi bo */
    ipc_cfg_storage_t *cfg_storage; /* NULL -> file "app.cfg" */
} app_cfg_t;

void app_cfg_default(app_cfg_t *cfg);

/* Khoi dong ca he: config -> health -> processor -> uploader -> sensor. */
bool app_start(const app_cfg_t *cfg);
void app_stop(void);

/*
 * Chay moi looper cho den khi khong con message nao den han. Tra ve tong so
 * message da xu ly. CHI dung khi spawn_tasks = false.
 *
 * Can lap nhieu vong vi mot su kien de nay ra su kien khac:
 * sensor -> processor -> uploader/config.
 */
uint32_t app_poll_all(void);

ipc_looper_t *app_looper(const char *service_name);

/* ---------------- tung dich vu ---------------- */

bool svc_config_start(const app_cfg_t *cfg);
bool svc_sensor_start(const app_cfg_t *cfg);
bool svc_processor_start(const app_cfg_t *cfg);
bool svc_uploader_start(const app_cfg_t *cfg);
bool svc_health_start(const app_cfg_t *cfg);

/* ---------------- cua so quan sat (test/chan doan) ---------------- */

int32_t  svc_sensor_peek_samples(void);
uint32_t svc_sensor_peek_fails(void);

int32_t  svc_processor_peek_avg(void);
uint32_t svc_processor_peek_alerts(void);

uint32_t svc_uploader_peek_pending(void);   /* so ban ghi dang cho day */
uint32_t svc_uploader_peek_uploaded(void);  /* tong so ban ghi da day thanh cong */
uint32_t svc_uploader_peek_dropped(void);   /* mat do hang doi tran */

uint32_t svc_config_peek_changes(void);

#ifdef __cplusplus
}
#endif
#endif /* APP_SERVICES_H */
