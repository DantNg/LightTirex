/*
 * ipc_watchdog.h - watchdog nhieu tang.
 *
 * Tang 1 (phan mem): moi "client" phai kick dinh ky. Qua han -> escalate
 *                    theo policy (chi log / khoi dong lai task / reset chip).
 * Tang 2 (phan cung): chi khi TAT CA client con khoe, watchdog core moi cho
 *                    hardware watchdog an. Neu chinh task watchdog chet hoac
 *                    treo, HW WDT khong duoc cho an -> chip tu reset.
 *                    Do la cau tra loi cho "ai canh nguoi gac den?".
 *
 * SOLID o day:
 *  - SRP     : core chi lo phat hien qua han. No khong biet reset chip lam
 *              sao, cung khong biet khoi dong lai task lam sao.
 *  - OCP/DIP : cach cho HW an nam sau ipc_wdt_backend_t; cach phuc hoi nam
 *              sau ipc_wdt_recovery_fn. Them nen tang moi (STM32 IWDG, nRF
 *              WDT, desktop gia lap) = viet them backend, khong sua core.
 *  - ISP     : backend chi 3 phuong thuc; ai lam backend khong phai biet gi
 *              ve looper hay message.
 *  - LSP     : backend gia lap tren desktop thay the duoc backend that,
 *              test kiem tra "da goi reset chua" thay vi that su reset.
 */
#ifndef IPC_WATCHDOG_H
#define IPC_WATCHDOG_H

#include "ipc_looper.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef IPC_MAX_WDT_CLIENTS
#define IPC_MAX_WDT_CLIENTS 12
#endif

#ifndef IPC_WDT_NAME_LEN
#define IPC_WDT_NAME_LEN 16
#endif

/* ---------------- backend phan cung ---------------- */

typedef struct ipc_wdt_backend {
    const char *name;
    /* Bat HW WDT voi thoi gian cho. Tra false neu nen tang khong ho tro. */
    bool (*init)(struct ipc_wdt_backend *self, uint32_t timeout_ms);
    /* Cho an. Phai duoc goi truoc khi het timeout_ms. */
    void (*feed)(struct ipc_wdt_backend *self);
    /* Reset he thong ngay lap tuc (khong tro ve). */
    void (*reset_system)(struct ipc_wdt_backend *self);
    void *impl;
} ipc_wdt_backend_t;

/* Khong lam gi ca - dung tren desktop hoac khi tam tat WDT phan cung. */
ipc_wdt_backend_t *ipc_wdt_backend_noop(void);

/* Ban gia lap cho test: dem so lan feed, ghi nhan da reset hay chua. */
typedef struct {
    ipc_wdt_backend_t base;
    uint32_t init_count;
    uint32_t feed_count;
    uint32_t reset_count;    /* test kiem tra so nay thay vi reset that */
    uint32_t last_timeout_ms;
} ipc_wdt_fake_backend_t;

void ipc_wdt_fake_backend_init(ipc_wdt_fake_backend_t *fb);

#ifdef ESP_PLATFORM
ipc_wdt_backend_t *ipc_wdt_backend_esp32(void);
#endif

/* ---------------- policy ---------------- */

typedef enum {
    IPC_WDT_POLICY_LOG = 0,     /* chi bao, khong dong cham */
    IPC_WDT_POLICY_RESTART,     /* goi recovery fn (thuong la supervisor) */
    IPC_WDT_POLICY_RESET,       /* reset ca he thong ngay */
} ipc_wdt_policy_t;

typedef struct ipc_wdt_client ipc_wdt_client_t;
typedef ipc_wdt_client_t *ipc_wdt_handle_t;

/* Ham phuc hoi duoc TIEM VAO. Core khong phu thuoc supervisor. Tra true neu
 * da xu ly duoc; false -> core leo thang len RESET. */
typedef bool (*ipc_wdt_recovery_fn)(const char *client, ipc_looper_t *lp, void *user);

/* Bao cao su kien de log/telemetry. */
typedef void (*ipc_wdt_event_fn)(const char *client, uint32_t overdue_ms,
                                 ipc_wdt_policy_t action, bool recovered,
                                 void *user);

typedef struct {
    ipc_wdt_backend_t *backend;      /* NULL -> noop */
    uint32_t hw_timeout_ms;          /* thoi gian cho cua HW WDT (0 = khong bat) */
    uint32_t check_interval_ms;      /* chu ky quet, mac dinh 500 */
    uint8_t  priority;               /* nen cao nhat he thong */
    uint32_t stack_words;
    bool     own_task;               /* false: tu goi ipc_wdt_check() (test) */
    /* So lan phuc hoi that bai lien tiep truoc khi reset ca he thong. 0 = khong bao gio reset. */
    uint32_t max_recovery_attempts;
    ipc_wdt_recovery_fn recovery;
    ipc_wdt_event_fn    on_event;
    void *user;
} ipc_wdt_cfg_t;

void ipc_wdt_cfg_default(ipc_wdt_cfg_t *cfg);
bool ipc_wdt_start(const ipc_wdt_cfg_t *cfg);
void ipc_wdt_stop(void);

/* ---------------- client ---------------- */

/* Client tu kick: dung cho vong lap khong phai looper (DMA pump, driver...). */
ipc_wdt_handle_t ipc_wdt_register(const char *name, uint32_t timeout_ms,
                                  ipc_wdt_policy_t policy);

/*
 * Client la mot looper: khong can kick tay, core doc heartbeat cua looper.
 * timeout_ms = 0 -> lay theo heartbeat_timeout_ms cua looper.
 */
ipc_wdt_handle_t ipc_wdt_watch_looper(ipc_looper_t *lp, uint32_t timeout_ms,
                                      ipc_wdt_policy_t policy);

void ipc_wdt_unregister(ipc_wdt_handle_t h);
void ipc_wdt_kick(ipc_wdt_handle_t h);

/* Tam dung giam sat mot client (vd sap lam viec dai nhu ghi flash/OTA). */
void ipc_wdt_suspend(ipc_wdt_handle_t h);
void ipc_wdt_resume(ipc_wdt_handle_t h);

/*
 * Quet mot lan: tra ve so client qua han. Cho HW an neu tat ca deu khoe.
 * Task watchdog goi ham nay; test goi truc tiep sau ipc_fake_clock_advance().
 */
uint32_t ipc_wdt_check(void);

typedef struct {
    uint32_t clients;
    uint32_t expiries;        /* tong so lan qua han */
    uint32_t recoveries_ok;
    uint32_t recoveries_failed;
    uint32_t feeds;
} ipc_wdt_stats_t;

void ipc_wdt_get_stats(ipc_wdt_stats_t *out);
void ipc_wdt_dump(void (*print)(const char *line));

#ifdef __cplusplus
}
#endif
#endif /* IPC_WATCHDOG_H */
