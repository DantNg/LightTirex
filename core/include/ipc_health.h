/*
 * ipc_health.h - dich vu suc khoe he thong.
 *
 * Khac biet voi watchdog: watchdog chi hoi mot cau "con song khong?".
 * Health service hoi "co dang om khong?" - no thu thap nhieu tin hieu
 * (heap, do sau hang doi, pool message, exception do code bao len) roi
 * QUYET DINH theo mot bang luat: bo qua / log / khoi dong lai dich vu /
 * giet han thread / reboot board.
 *
 * SRP  : health chi thu thap + quyet dinh. No khong biet cach reset chip
 *        (muon backend cua watchdog), khong biet cach hoi sinh task
 *        (muon supervisor qua ham tiem vao).
 * OCP  : them tin hieu moi = them mot probe; them cach xu ly = them mot
 *        luat trong bang, khong sua ipc_health.c.
 * DIP  : doc tai nguyen qua ipc_health_probe_t nen tren desktop ta bom
 *        so lieu gia de test "het heap thi lam gi" ma khong can het heap that.
 */
#ifndef IPC_HEALTH_H
#define IPC_HEALTH_H

#include "ipc_looper.h"
#include "ipc_watchdog.h"   /* dung lai ipc_wdt_backend_t de reset he thong */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef IPC_HEALTH_MAX_RULES
#define IPC_HEALTH_MAX_RULES 16
#endif

#ifndef IPC_HEALTH_MAX_SOURCES
#define IPC_HEALTH_MAX_SOURCES 12
#endif

/* Kich thuoc vong dem exception. Bao loi tu ISR chi ghi vao day roi ve ngay. */
#ifndef IPC_HEALTH_RING_SIZE
#define IPC_HEALTH_RING_SIZE 32
#endif

#ifndef IPC_HEALTH_NAME_LEN
#define IPC_HEALTH_NAME_LEN 16
#endif

typedef enum {
    IPC_SEV_INFO = 0,
    IPC_SEV_WARN,
    IPC_SEV_ERROR,
    IPC_SEV_FATAL,
} ipc_severity_t;

/* Ma exception dung san. Nguoi dung tu dinh nghia them tu IPC_EXC_USER_BASE. */
enum {
    IPC_EXC_NONE = 0,
    IPC_EXC_ASSERT,        /* dieu kien bat bien bi vi pham */
    IPC_EXC_OOM,           /* het bo nho / het pool */
    IPC_EXC_TIMEOUT,       /* cho qua lau, khong co phan hoi */
    IPC_EXC_HW_FAULT,      /* loi phan cung: I2C nack, SPI timeout... */
    IPC_EXC_QUEUE_FULL,    /* hang doi looper qua sau */
    IPC_EXC_LOW_HEAP,
    IPC_EXC_TASK_HUNG,
    IPC_EXC_PROTOCOL,      /* du lieu vao khong hop le */
    IPC_EXC_USER_BASE = 1000,
};

#define IPC_EXC_ANY 0xFFFFFFFFu

typedef enum {
    IPC_ACT_NONE = 0,
    IPC_ACT_LOG,               /* ghi nhan, khong dong cham */
    IPC_ACT_RESTART_SERVICE,   /* goi ham phuc hoi (thuong la supervisor) */
    IPC_ACT_KILL_SERVICE,      /* giet han thread, KHONG hoi sinh */
    IPC_ACT_SAFE_MODE,         /* ha he thong ve che do toi thieu */
    IPC_ACT_REBOOT,            /* reset ca board */
} ipc_health_action_t;

/*
 * Mot luat. Duyet theo dung thu tu dang ky, luat DAU TIEN khop se thang -
 * nen dang ky luat hep truoc, luat rong sau.
 *
 * Y nghia threshold: can <count> lan khop trong <window_ms> thi moi hanh dong.
 * count = 1 nghia la hanh dong ngay lan dau.
 */
typedef struct {
    uint32_t       code;        /* IPC_EXC_ANY = moi ma */
    const char    *source;      /* NULL = moi nguon */
    ipc_severity_t min_severity;
    uint32_t       count;
    uint32_t       window_ms;   /* 0 = khong gioi han thoi gian */
    ipc_health_action_t action;
} ipc_health_rule_t;

/* Doc tai nguyen he thong. Tren desktop/test cai ban gia de bom so lieu. */
typedef struct ipc_health_probe {
    const char *name;
    uint32_t (*free_heap)(struct ipc_health_probe *self);
    uint32_t (*min_free_heap)(struct ipc_health_probe *self);  /* thap nhat tung ghi nhan */
    void *impl;
} ipc_health_probe_t;

/* Probe that cua nen tang (ESP-IDF/FreeRTOS heap, hoac desktop tra 0). */
ipc_health_probe_t *ipc_health_probe_platform(void);

/* Probe gia cho test: tu dat so lieu. */
typedef struct {
    ipc_health_probe_t base;
    uint32_t heap;
    uint32_t min_heap;
} ipc_health_fake_probe_t;
void ipc_health_fake_probe_init(ipc_health_fake_probe_t *fp, uint32_t heap);

/* Bao cao mot lan hanh dong da xay ra. */
typedef void (*ipc_health_event_fn)(const char *source, uint32_t code,
                                    ipc_severity_t sev, ipc_health_action_t act,
                                    int32_t detail, void *user);

/* Ham phuc hoi duoc tiem vao - giong watchdog, health khong biet supervisor. */
typedef bool (*ipc_health_recovery_fn)(const char *service, ipc_looper_t *lp, void *user);

typedef struct {
    ipc_health_probe_t *probe;      /* NULL -> probe nen tang */
    ipc_wdt_backend_t  *backend;    /* de reset he thong; NULL -> noop */
    ipc_health_recovery_fn recovery;
    ipc_health_event_fn    on_event;
    void (*on_safe_mode)(void *user);

    uint32_t heap_warn_bytes;       /* duoi muc nay -> exception LOW_HEAP/WARN */
    uint32_t heap_critical_bytes;   /* duoi muc nay -> LOW_HEAP/ERROR */
    uint32_t queue_depth_warn;      /* hang doi looper sau hon muc nay -> QUEUE_FULL */
    uint32_t pool_free_warn;        /* so message con trong pool duoi muc nay -> OOM */

    uint32_t check_interval_ms;
    uint8_t  priority;
    uint32_t stack_words;
    bool     own_task;              /* false: tu goi ipc_health_check() (test) */
    void    *user;
} ipc_health_cfg_t;

void ipc_health_cfg_default(ipc_health_cfg_t *cfg);
bool ipc_health_start(const ipc_health_cfg_t *cfg);
void ipc_health_stop(void);

/* Dang ky luat. Tra false neu het slot. Thu tu dang ky = thu tu uu tien. */
bool ipc_health_add_rule(const ipc_health_rule_t *rule);
void ipc_health_clear_rules(void);

/*
 * Bao mot exception. RE va an toan goi tu bat ky dau, ke ca ISR: chi ghi vao
 * vong dem roi ve. Viec danh gia luat lam o task health.
 * detail: so lieu kem theo tuy nguoi goi (ma loi driver, so byte thieu...).
 */
void ipc_health_report(const char *source, uint32_t code, ipc_severity_t sev,
                       int32_t detail);

/* Mot nhip khong chan: rut vong dem, doc probe, ap luat, thi hanh. Tra ve so
 * hanh dong da thuc hien. Task health goi trong vong lap; test goi truc tiep. */
uint32_t ipc_health_check(void);

typedef struct {
    uint32_t uptime_ms;
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint32_t pool_free;
    uint32_t exceptions[4];        /* dem theo muc do nghiem trong */
    uint32_t dropped_reports;      /* vong dem day -> mat bao cao */
    uint32_t actions_taken;
    uint32_t restarts;
    uint32_t reboots;
    ipc_health_action_t last_action;
    char     last_source[IPC_HEALTH_NAME_LEN];
    bool     degraded;             /* dang co van de chua giai quyet */
} ipc_health_status_t;

void ipc_health_get_status(ipc_health_status_t *out);
void ipc_health_dump(void (*print)(const char *line));

/* Xoa trang thai "degraded" sau khi da xu ly xong. */
void ipc_health_clear_degraded(void);

#ifdef __cplusplus
}
#endif
#endif /* IPC_HEALTH_H */
