/*
 * app_events.h - BAN HOP DONG chung cua he thong IoT nay.
 *
 * Moi thu cac dich vu dung de noi chuyen voi nhau deu nam o day: ten dich vu,
 * chu de su kien, co dong bo, khoa cau hinh. Cac file svc_*.c KHONG include
 * lan nhau - chung chi cung include file nay.
 *
 * Do la diem mau chot: uploaderService khong biet sensorService ton tai. No chi
 * biet chu de TOPIC_DATA_READY. Thay cam bien khac, them mot nguon du lieu
 * thu hai, hay bo hoan uploader - khong file nao khac phai sua.
 *
 *   [sensor] --SENSOR_SAMPLE--> [processor] --DATA_READY--> [uploader]
 *                                    |                          |
 *                                    +--------DATA_READY-------> [config]
 *                                    |                       (luu xuong file)
 *                                    +--ALERT--> [health] --HEALTH_STATE-->
 *
 *   [config] --CONFIG_CHANGED--> (sensor doi chu ky lay mau, uploader doi lo)
 */
#ifndef APP_EVENTS_H
#define APP_EVENTS_H

#include "ipc_looper.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- ten dich vu (tra cuu qua ServiceManager) ---------------- */

#define SVC_SENSOR    "sensor"
#define SVC_PROCESSOR "processor"
#define SVC_CONFIG    "config"
#define SVC_UPLOADER  "uploader"
#define SVC_HEALTH    "health"

/* ---------------- chu de su kien (event bus) ---------------- */

enum {
    TOPIC_SENSOR_SAMPLE = 1,  /* arg1 = milli-do C, arg2 = so thu tu mau */
    TOPIC_DATA_READY,         /* arg1 = trung binh truot, arg2 = so mau da gop */
    TOPIC_ALERT,              /* arg1 = gia tri vi pham, arg2 = 1 cao / -1 thap */
    TOPIC_CONFIG_CHANGED,     /* arg1 = ma khoa (CFGK_*), arg2 = gia tri moi */
    TOPIC_NET_STATE,          /* arg1 = 1 online / 0 offline (giu lai) */
    TOPIC_UPLOAD_RESULT,      /* arg1 = so ban ghi, arg2 = 1 ok / 0 that bai */
    TOPIC_HEALTH_STATE,       /* arg1 = 1 khoe / 0 co van de (giu lai) */
};

/* Ma khoa cau hinh, de nhet vua vao arg1 cua su kien. */
enum {
    CFGK_SAMPLE_PERIOD_MS = 1,
    CFGK_UPLOAD_BATCH,
    CFGK_ALERT_HIGH_MC,
    CFGK_ALERT_LOW_MC,
    CFGK_OTHER,
};

/* ---------------- ma message noi bo tung dich vu ---------------- */

enum {
    /* Dung cho message tu bus: moi dang ky chon mot ma rieng de phan biet. */
    MSG_EV_SENSOR_SAMPLE = 100,
    MSG_EV_DATA_READY,
    MSG_EV_CONFIG_CHANGED,
    MSG_EV_ALERT,
    MSG_EV_NET_STATE,

    /* Message tu timer / noi bo. */
    MSG_SENSOR_TICK = 200,
    MSG_UPLOAD_FLUSH,
    MSG_UPLOAD_RETRY,
    MSG_PERSIST,
};

/* ---------------- khoa doc/ghi cua tung dich vu ----------------
 *
 * Dung voi svc->get()/svc->set(), hoac duong tat app_service_get/set().
 * Moi dich vu co khong gian khoa rieng, bat dau tu 1.
 */

enum {                        /* SVC_CONFIG */
    CFGK_PERIOD_MS = 1,
    CFGK_UPLOAD_BATCH_N,
    CFGK_ALERT_HIGH,
    CFGK_ALERT_LOW,
    CFGK_LAST_VALUE,
    CFGK_CHANGE_COUNT,        /* chi doc */
};

enum {                        /* SVC_SENSOR */
    SENSORK_SAMPLES = 1,      /* chi doc: so mau da doc duoc */
    SENSORK_FAILS,            /* chi doc: so lan doc hong */
    SENSORK_FAIL_STREAK,      /* chi doc: so lan hong lien tiep hien tai */
};

enum {                        /* SVC_PROCESSOR */
    PROCK_LAST_AVG = 1,       /* chi doc */
    PROCK_ALERTS,             /* chi doc */
    PROCK_PROCESSED,          /* chi doc */
};

enum {                        /* SVC_UPLOADER */
    UPK_PENDING = 1,          /* chi doc: so ban ghi dang cho day */
    UPK_UPLOADED,             /* chi doc: tong da day thanh cong */
    UPK_DROPPED,              /* chi doc: mat do hang doi tran */
    UPK_ONLINE,               /* doc/ghi: 1 = coi nhu co mang */
};

enum {                        /* SVC_HEALTH */
    HEALTHK_DEGRADED = 1,     /* chi doc: 1 = dang co van de */
    HEALTHK_ALERTS,           /* chi doc: so canh bao nguong da thay */
};

/* ---------------- khoa cau hinh (ten trong file) ---------------- */

#define CFG_SAMPLE_PERIOD "sample.period_ms"
#define CFG_UPLOAD_BATCH  "upload.batch"
#define CFG_ALERT_HIGH    "alert.high_mc"
#define CFG_ALERT_LOW     "alert.low_mc"
#define CFG_DEVICE_NAME   "device.name"
#define CFG_LAST_VALUE    "data.last_mc"
#define CFG_SAMPLE_COUNT  "data.count"

/* ---------------- co dong bo (event group) ---------------- */

enum {
    BIT_CONFIG_READY    = 1u << 0,
    BIT_SENSOR_READY    = 1u << 1,
    BIT_PROCESSOR_READY = 1u << 2,
    BIT_UPLOADER_READY  = 1u << 3,
    BIT_HEALTH_READY    = 1u << 4,
    BIT_NET_ONLINE      = 1u << 5,
    BIT_ALL_SERVICES    = BIT_CONFIG_READY | BIT_SENSOR_READY |
                          BIT_PROCESSOR_READY | BIT_UPLOADER_READY |
                          BIT_HEALTH_READY,
};

/* Nhom co dung chung. Do app.c tao. */
struct ipc_event_group *app_bits(void);

/* ---------------- ma exception bao len health ---------------- */

enum {
    EXC_SENSOR_READ = 1000,   /* doc cam bien that bai */
    EXC_UPLOAD_FAIL,          /* day len server that bai */
    EXC_QUEUE_OVERFLOW,       /* hang doi ngoai tuyen tran, mat du lieu */
    EXC_CFG_CORRUPT,          /* file cau hinh hong */
};

#ifdef __cplusplus
}
#endif
#endif /* APP_EVENTS_H */
