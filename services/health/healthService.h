/*
 * healthService.h - bang luat xu ly su co cua ung dung
 *
 * Dich vu chi lo ra DUNG MOT ham: lay bang mo ta cua no. Moi thu khac
 * (nhan message, doc/ghi tham so, khoi tao lai) di qua khuon chung trong
 * common/service_iface.h.
 */
#ifndef HEALTHSERVICE_H
#define HEALTHSERVICE_H

#include "common/service_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

app_service_t *healthService(void);

#ifdef __cplusplus
}
#endif
#endif /* HEALTHSERVICE_H */
