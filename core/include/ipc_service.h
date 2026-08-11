/*
 * ipc_service.h - ServiceManager: tra cuu handler theo TEN.
 *
 * Ly do ton tai: sau khi mot task chet va duoc hoi sinh, con tro handler cu
 * co the khong con hop le. Client khong nen giu con tro tho - ho tra cuu
 * theo ten, giong nhu getService("audio") tren Android. Lop nay cung cho
 * phep dang ky "death recipient" (linkToDeath) de client biet dich vu vua
 * duoc khoi dong lai va tu dong ket noi lai.
 */
#ifndef IPC_SERVICE_H
#define IPC_SERVICE_H

#include "ipc_looper.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef IPC_MAX_SERVICES
#define IPC_MAX_SERVICES 16
#endif

#ifndef IPC_MAX_DEATH_RECIPIENTS
#define IPC_MAX_DEATH_RECIPIENTS 16
#endif

typedef struct ipc_service ipc_service_t;

/* Duoc goi khi dich vu chet (alive=false) va khi song lai (alive=true).
 * Chay tren task supervisor -> khong duoc block lau. */
typedef void (*ipc_death_recipient_fn)(const char *service, bool alive,
                                       uint32_t generation, void *user);

void ipc_service_manager_init(void);

/* Dang ky handler duoi mot ten. Ghi de neu ten da ton tai. */
bool ipc_service_register(const char *name, ipc_handler_t *h);
void ipc_service_unregister(const char *name);

/* Tra cuu. NULL neu chua dang ky hoac dich vu dang chet. */
ipc_handler_t *ipc_service_get(const char *name);

/* Gui thang theo ten - khong can biet handler. */
bool ipc_service_send(const char *name, uint32_t what, int32_t arg1, int32_t arg2);
bool ipc_service_send_msg(const char *name, ipc_message_t *msg);
bool ipc_service_call_sync(const char *name, uint32_t what, int32_t arg1,
                           void *payload, uint32_t timeout_ms);

/* linkToDeath / unlinkToDeath. */
bool ipc_service_link_to_death(const char *name, ipc_death_recipient_fn fn, void *user);
void ipc_service_unlink_to_death(const char *name, ipc_death_recipient_fn fn, void *user);

/* Supervisor goi de phat tin. */
void ipc_service_notify_state(ipc_looper_t *lp, bool alive);

#ifdef __cplusplus
}
#endif
#endif /* IPC_SERVICE_H */
