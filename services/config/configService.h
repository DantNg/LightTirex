/*
 * configService.h - giu cau hinh + luu du lieu xuong file
 *
 * Dich vu chi lo ra DUNG MOT ham: lay bang mo ta cua no. Moi thu khac
 * (nhan message, doc/ghi tham so, khoi tao lai) di qua khuon chung trong
 * common/service_iface.h.
 */
#ifndef CONFIGSERVICE_H
#define CONFIGSERVICE_H

#include "common/service_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

app_service_t *configService(void);

#ifdef __cplusplus
}
#endif
#endif /* CONFIGSERVICE_H */
