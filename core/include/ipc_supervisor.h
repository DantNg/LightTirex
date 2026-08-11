/*
 * ipc_supervisor.h - vai tro cua `init` tren Linux: quet cac looper, phat
 * hien task chet/treo va hoi sinh chung ngay luc runtime.
 */
#ifndef IPC_SUPERVISOR_H
#define IPC_SUPERVISOR_H

#include "ipc_looper.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ipc_supervisor_log_fn)(const char *service, ipc_death_reason_t why,
                                      uint32_t restart_count, bool revived);

typedef struct {
    uint32_t scan_interval_ms; /* mac dinh 500 */
    uint8_t  priority;         /* nen CAO hon moi looper duoc giam sat */
    uint32_t stack_words;
    ipc_supervisor_log_fn on_event;
} ipc_supervisor_cfg_t;

void ipc_supervisor_cfg_default(ipc_supervisor_cfg_t *cfg);
bool ipc_supervisor_start(const ipc_supervisor_cfg_t *cfg);
void ipc_supervisor_stop(void);

/* Ep khoi dong lai mot looper ngay lap tuc (dung cho test / OTA / recovery). */
bool ipc_supervisor_force_restart(ipc_looper_t *lp);

/* Quet mot lan dong bo - dung khi khong muon task supervisor rieng. */
void ipc_supervisor_scan_once(void);

#ifdef __cplusplus
}
#endif
#endif /* IPC_SUPERVISOR_H */
