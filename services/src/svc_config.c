/*
 * svc_config.c - cau hinh he thong + luu du lieu do duoc.
 *
 * Hai vai tro, deu xoay quanh mot cai file:
 *   1. Giu cau hinh. Ai doi mot khoa thi no cong bo TOPIC_CONFIG_CHANGED,
 *      cac dich vu khac tu dieu chinh - khong ai phai khoi dong lai.
 *   2. Nghe TOPIC_DATA_READY va luu gia tri moi nhat xuong file.
 *
 * Ve tan suat ghi: du lieu ve lien tuc nhung flash chi chiu duoc so lan xoa
 * co han, nen khong ghi tung mau - ipc_config gop cac lan set lai va chi ghi
 * mot lan sau khoang lang.
 */
#include "app.h"
#include "services.h"

#include "ipc_event.h"
#include "ipc_health.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t changes;
    uint32_t samples_stored;
} config_state_t;

static config_state_t s_state;

static const ipc_cfg_schema_t k_schema[] = {
    { CFG_SAMPLE_PERIOD, IPC_CFG_INT,  1000,  NULL      },
    { CFG_UPLOAD_BATCH,  IPC_CFG_INT,  4,     NULL      },
    { CFG_ALERT_HIGH,    IPC_CFG_INT,  30000, NULL      },  /* 30.0 do C */
    { CFG_ALERT_LOW,     IPC_CFG_INT,  15000, NULL      },  /* 15.0 do C */
    { CFG_DEVICE_NAME,   IPC_CFG_STR,  0,     "tirex-1" },
    { CFG_LAST_VALUE,    IPC_CFG_INT,  0,     NULL      },
    { CFG_SAMPLE_COUNT,  IPC_CFG_INT,  0,     NULL      },
};

/* Doi khoa so (dung trong su kien) <-> ten khoa (trong file). */
static const char *key_name(uint32_t key)
{
    switch (key) {
    case CFGK_PERIOD_MS:       return CFG_SAMPLE_PERIOD;
    case CFGK_UPLOAD_BATCH_N:  return CFG_UPLOAD_BATCH;
    case CFGK_ALERT_HIGH:      return CFG_ALERT_HIGH;
    case CFGK_ALERT_LOW:       return CFG_ALERT_LOW;
    case CFGK_LAST_VALUE:      return CFG_LAST_VALUE;
    default:                   return NULL;
    }
}

static int32_t name_key(const char *name)
{
    if (strcmp(name, CFG_SAMPLE_PERIOD) == 0) return CFGK_PERIOD_MS;
    if (strcmp(name, CFG_UPLOAD_BATCH) == 0)  return CFGK_UPLOAD_BATCH_N;
    if (strcmp(name, CFG_ALERT_HIGH) == 0)    return CFGK_ALERT_HIGH;
    if (strcmp(name, CFG_ALERT_LOW) == 0)     return CFGK_ALERT_LOW;
    return 0;   /* du lieu do dac, khong phai cau hinh -> khong cong bo */
}

/* ---------------- moc doi ---------------- */

/* ipc_config goi tren context cua nguoi set. Chi cong bo (day message vao
 * hang doi), khong lam viec nang o day. */
static void on_cfg_write(const char *name, void *user)
{
    (void)user;
    int32_t key = name_key(name);
    if (key == 0) return;

    s_state.changes++;
    ipc_bus_publish(TOPIC_CONFIG_CHANGED, key, ipc_cfg_get_int(name, 0));
}

static void config_on_create(app_service_t *self)
{
    const app_cfg_t *acfg = (const app_cfg_t *)self->ctx;
    self->state = &s_state;

    ipc_cfg_cfg_t c;
    ipc_cfg_cfg_default(&c);
    c.storage = acfg->cfg_storage ? acfg->cfg_storage
                                  : ipc_cfg_storage_file("app.cfg");
    c.schema = k_schema;
    c.schema_count = sizeof(k_schema) / sizeof(k_schema[0]);
    c.on_change = on_cfg_write;
    c.autosave_delay_ms = 500;      /* gop nhieu lan set thanh mot lan ghi */
    c.writer_looper = self->looper; /* ghi file tren looper nay, khong nghen timer */

    if (ipc_cfg_init(&c) == IPC_CFG_ERR_CORRUPT)
        ipc_health_report(SVC_CONFIG, EXC_CFG_CORRUPT, IPC_SEV_WARN, 0);

    s_state.samples_stored = (uint32_t)ipc_cfg_get_int(CFG_SAMPLE_COUNT, 0);
}

static void config_on_subscribe(app_service_t *self)
{
    (void)self;
    ipc_bus_subscribe_service(TOPIC_DATA_READY, SVC_CONFIG, MSG_EV_DATA_READY);
}

static bool config_on_receive(app_service_t *self, ipc_message_t *msg)
{
    (void)self;
    if (msg->what == MSG_EV_DATA_READY) {
        /* Luu gia tri moi nhat de sau khi mat dien con biet lan cuoi doc gi. */
        ipc_cfg_set_int(CFG_LAST_VALUE, msg->arg1);
        s_state.samples_stored++;
        ipc_cfg_set_int(CFG_SAMPLE_COUNT, (int32_t)s_state.samples_stored);
    }
    return true;
}

/* ---------------- doc/ghi tham so ---------------- */

static int32_t config_get(app_service_t *self, uint32_t key, int32_t def)
{
    (void)self;
    if (key == CFGK_CHANGE_COUNT) return (int32_t)s_state.changes;
    const char *name = key_name(key);
    if (!name) return SVC_VALUE_INVALID;
    return ipc_cfg_get_int(name, def);
}

static bool config_set(app_service_t *self, uint32_t key, int32_t value)
{
    (void)self;
    const char *name = key_name(key);
    if (!name) return false;
    return ipc_cfg_set_int(name, value) == IPC_CFG_OK;
}

/* ---------------- bang mo ta ---------------- */

static app_service_t s_svc = {
    .name = SVC_CONFIG,
    .priority = 6,
    .heartbeat_timeout_ms = 5000,
    .ready_bit = BIT_CONFIG_READY,
    .on_create = config_on_create,
    .on_subscribe = config_on_subscribe,
    .on_receive = config_on_receive,
    .get = config_get,
    .set = config_set,
};

app_service_t *svc_config(void) { return &s_svc; }
