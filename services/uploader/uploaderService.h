/*
 * uploaderService.h - gom lo va day len server
 *
 * Dich vu chi lo ra DUNG MOT ham: lay bang mo ta cua no. Moi thu khac
 * (nhan message, doc/ghi tham so, khoi tao lai) di qua khuon chung trong
 * common/service_iface.h.
 */
#ifndef UPLOADERSERVICE_H
#define UPLOADERSERVICE_H

#include "common/service_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

app_service_t *uploaderService(void);

#ifdef __cplusplus
}
#endif
#endif /* UPLOADERSERVICE_H */
