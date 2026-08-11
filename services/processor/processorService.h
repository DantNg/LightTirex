/*
 * processorService.h - loc nhieu, phat hien vuot nguong
 *
 * Dich vu chi lo ra DUNG MOT ham: lay bang mo ta cua no. Moi thu khac
 * (nhan message, doc/ghi tham so, khoi tao lai) di qua khuon chung trong
 * common/service_iface.h.
 */
#ifndef PROCESSORSERVICE_H
#define PROCESSORSERVICE_H

#include "common/service_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

app_service_t *processorService(void);

#ifdef __cplusplus
}
#endif
#endif /* PROCESSORSERVICE_H */
