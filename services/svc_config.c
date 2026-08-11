/*
 * svc_config.c - dich vu cau hinh + luu du lieu.
 *
 * Hai vai tro, deu xoay quanh mot cai file:
 *   1. Giu cau hinh he thong. Ai doi mot khoa thi no cong bo len bus, cac
 *      dich vu khac tu dieu chinh - khong ai phai khoi dong lai.
 *   2. Nghe TOPIC_DATA_READY va luu gia tri moi nhat xuong file.
 *
 * Luu y ve tan suat ghi: du lieu ve lien tuc nhung flash chi chiu duoc
 * so lan xoa co han. Vi vay ta khong ghi moi mau - ipc_config gop cac lan
 * set lai va chi ghi mot lan sau khoang lang.
 */
#include "services.h"

#include "ipc_event.h"
#include "ipc_health.h"
#include "ipc_service.h"
#include "ipc_timer.h"

#include <stdio.h>
#include <string.h>

static ipc_looper_t  *s_lp;
static ipc_handler_t  s_h;
static uint32_t       s_changes;
static uint32_t       s_samples_stored;

static const ipc_cfg_schema_t k_schema[] = {
    { CFG_SAMPLE_PERIOD, IPC_CFG_INT,  1000,  NULL      },
    { CFG_UPLOAD_BATCH,  IPC_CFG_INT,  4,     NULL      },
    { CFG_ALERT_HIGH,    IPC_CFG_INT,  30000, NULL      },  /* 30.0 do C */
    { CFG_ALERT_LOW,     IPC_CFG_INT,  15000, NULL      },  /* 15.0 do C */
    { CFG_DEVICE_NAME,   IPC_CFG_STR,  0,     "tirex-1" },
    { CFG_LAST_VALUE,    IPC_CFG_INT,  0,     NULL      },
    { CFG_SAMPLE_COUNT,  IPC_CFG_INT,  0,     NULL      },
};

/* Doi ten khoa -> ma so de nhet vua vao mot su kien. */
static int32_t key_code(const char *key)
{
    if (strcmp(key, CFG_SAMPLE_PERIOD) == 0) return CFGK_SAMPLE_PERIOD_MS;
    if (strcmp(key, CFG_UPLOAD_BATCH) == 0)  return CFGK_UPLOAD_BATCH;
    if (strcmp(key, CFG_ALERT_HIGH) == 0)    return CFGK_ALERT_HIGH_MC;
    if (strcmp(key, CFG_ALERT_LOW) == 0)     return CFGK_ALERT_LOW_MC;
    return CFGK_OTHER;
}

/*
 * ipc_config goi ham nay tren context cua nguoi set. Ta chi cong bo len bus
 * (chi la day message vao hang doi) chu khong lam vic nang o day.
 */
static void on_cfg_change(const char *key, void *user)
{
    (void)user;
    int32_t code = key_code(key);
    if (code == CFGK_OTHER) return;   /* du lieu do dac, khong phai cau hinh */

    s_changes++;
    ipc_bus_publish(TOPIC_CONFIG_CHANGED, code, ipc_cfg_get_int(key, 0));
}

static bool config_cb(ipc_handler_t *h, ipc_message_t *m, void *user)
{
    (void)h; (void)user;
    if (m->what == MSG_EV_DATA_READY) {
        /* arg1 = trung binh truot. Luu lai de sau khi mat dien con biet
         * lan cuoi doc duoc gi. */
        ipc_cfg_set_int(CFG_LAST_VALUE, m->arg1);
        s_samples_stored++;
        ipc_cfg_set_int(CFG_SAMPLE_COUNT, (int32_t)s_samples_stored);
    }
    return true;
}

static void config_on_start(ipc_looper_t *lp, void *user)
{
    const app_cfg_t *acfg = (const app_cfg_t *)user;

    /* Chay lai moi lan hoi sinh -> phai don dang ky cu truoc, neu khong se
     * co hai ban ghi nghe cung mot chu de. */
    ipc_bus_unsubscribe_service(SVC_CONFIG);

    ipc_handler_init(&s_h, lp, config_cb, NULL, SVC_CONFIG);
    ipc_service_register(SVC_CONFIG, &s_h);

    ipc_cfg_cfg_t c;
    ipc_cfg_cfg_default(&c);
    c.storage = acfg->cfg_storage ? acfg->cfg_storage
                                  : ipc_cfg_storage_file("app.cfg");
    c.schema = k_schema;
    c.schema_count = sizeof(k_schema) / sizeof(k_schema[0]);
    c.on_change = on_cfg_change;
    c.autosave_delay_ms = 500;   /* gop nhieu lan set thanh mot lan ghi */
    c.writer_looper = lp;        /* ghi file tren looper nay, khong nghen timer */

    if (ipc_cfg_init(&c) == IPC_CFG_ERR_CORRUPT)
        ipc_health_report(SVC_CONFIG, EXC_CFG_CORRUPT, IPC_SEV_WARN, 0);

    s_samples_stored = (uint32_t)ipc_cfg_get_int(CFG_SAMPLE_COUNT, 0);

    /* Dang ky nghe theo TEN: dich vu nay chet roi song lai van nhan tiep. */
    ipc_bus_subscribe_service(TOPIC_DATA_READY, SVC_CONFIG, MSG_EV_DATA_READY);

    ipc_event_group_set(app_bits(), BIT_CONFIG_READY);
}

bool svc_config_start(const app_cfg_t *cfg)
{
    ipc_looper_cfg_t lc;
    ipc_looper_cfg_default(&lc, SVC_CONFIG);
    lc.priority = 6;
    lc.heartbeat_timeout_ms = 5000;
    lc.on_start = config_on_start;
    lc.user = (void *)cfg;
    lc.purge_queue_on_restart = false;

    s_lp = ipc_looper_create(&lc);
    if (!s_lp) return false;

    if (cfg->spawn_tasks) return ipc_looper_start(s_lp);
    config_on_start(s_lp, (void *)cfg);   /* khong co task -> tu goi */
    return true;
}

uint32_t svc_config_peek_changes(void) { return s_changes; }
