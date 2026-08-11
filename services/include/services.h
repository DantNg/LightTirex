/*
 * services.h - danh sach cac dich vu cua ung dung.
 *
 * Moi dich vu chi lo ra DUNG MOT ham: lay bang mo ta cua no. Toan bo cach
 * doc/ghi tham so, cach nhan message, cach khoi tao lai deu di qua khuon
 * chung trong service_iface.h.
 *
 * Muon them dich vu moi: viet mot file svc_xxx.c theo khuon, khai bao
 * svc_xxx() o day, va them mot dong vao bang trong app.c. Khong sua gi khac.
 */
#ifndef APP_SERVICES_H
#define APP_SERVICES_H

#include "app_events.h"
#include "drivers.h"
#include "service_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

app_service_t *svc_config(void);
app_service_t *svc_sensor(void);
app_service_t *svc_processor(void);
app_service_t *svc_uploader(void);
app_service_t *svc_health(void);

#ifdef __cplusplus
}
#endif
#endif /* APP_SERVICES_H */
