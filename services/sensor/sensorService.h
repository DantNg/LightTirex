/*
 * sensorService.h - doc cam bien theo chu ky, cong bo mau len bus
 *
 * Dich vu chi lo ra DUNG MOT ham: lay bang mo ta cua no. Moi thu khac
 * (nhan message, doc/ghi tham so, khoi tao lai) di qua khuon chung trong
 * common/service_iface.h.
 */
#ifndef SENSORSERVICE_H
#define SENSORSERVICE_H

#include "common/service_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

app_service_t *sensorService(void);

#ifdef __cplusplus
}
#endif
#endif /* SENSORSERVICE_H */
