/*
 * services.h - danh sach cac dich vu cua ung dung.
 *
 * Moi dich vu nam trong mot thu muc rieng, lo ra dung mot ham lay bang mo ta.
 *
 * Muon them dich vu moi:
 *   1. tao services/<ten>/<ten>Service.{h,c} theo khuon service_iface.h
 *   2. them mot dong #include o day
 *   3. them mot dong vao bang k_services[] trong app/app.c
 * Khong sua gi khac.
 */
#ifndef APP_SERVICES_H
#define APP_SERVICES_H

#include "common/app_events.h"
#include "common/service_iface.h"
#include "drivers/drivers.h"

#include "config/configService.h"
#include "health/healthService.h"
#include "processor/processorService.h"
#include "sensor/sensorService.h"
#include "uploader/uploaderService.h"

#endif /* APP_SERVICES_H */
