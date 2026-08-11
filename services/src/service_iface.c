#include "service_iface.h"

#include "app_events.h"
#include "ipc_event.h"
#include "ipc_event_group.h"
#include "ipc_service.h"

#include <string.h>

#ifndef APP_MAX_SERVICES
#define APP_MAX_SERVICES 8
#endif

static app_service_t *s_registry[APP_MAX_SERVICES];
static uint32_t       s_count;

static void registry_add(app_service_t *svc)
{
    for (uint32_t i = 0; i < s_count; ++i)
        if (s_registry[i] == svc) return;
    if (s_count < APP_MAX_SERVICES) s_registry[s_count++] = svc;
}

app_service_t *app_service_find(const char *name)
{
    if (!name) return NULL;
    for (uint32_t i = 0; i < s_count; ++i)
        if (strcmp(s_registry[i]->name, name) == 0) return s_registry[i];
    return NULL;
}

/* Mot cua duy nhat cho moi message cua moi dich vu. */
static bool service_dispatch(ipc_handler_t *h, ipc_message_t *msg, void *user)
{
    app_service_t *svc = (app_service_t *)user;
    (void)h;
    if (!svc || !svc->on_receive) return true;
    return svc->on_receive(svc, msg);
}

/*
 * Chay trong task cua dich vu, va chay LAI moi lan dich vu duoc hoi sinh.
 * Thu tu o day la hop dong - xem so do trong service_iface.h.
 */
static void service_on_start(ipc_looper_t *lp, void *user)
{
    app_service_t *svc = (app_service_t *)user;

    /* 1. Handler moi cho vong doi moi, roi cong bo ten. */
    ipc_handler_init(&svc->handler, lp, service_dispatch, svc, svc->name);
    ipc_service_register(svc->name, &svc->handler);

    /* 2. Don dang ky nghe cua vong doi TRUOC. Neu bo buoc nay, sau moi lan
     *    hoi sinh dich vu se nhan moi su kien hai lan. */
    ipc_bus_unsubscribe_service(svc->name);

    /* 3-4. Phan rieng cua tung dich vu. */
    if (svc->on_create)    svc->on_create(svc);
    if (svc->on_subscribe) svc->on_subscribe(svc);

    /* 5. Bao voi ca he thong la da san sang. */
    if (svc->ready_bit && app_bits())
        ipc_event_group_set(app_bits(), svc->ready_bit);
}

bool app_service_start(app_service_t *svc, void *ctx, bool spawn_task)
{
    if (!svc || !svc->name) return false;
    svc->ctx = ctx;
    registry_add(svc);

    ipc_looper_cfg_t lc;
    ipc_looper_cfg_default(&lc, svc->name);
    lc.priority = svc->priority ? svc->priority : 5;
    lc.stack_words = svc->stack_words ? svc->stack_words : 3072;
    lc.heartbeat_timeout_ms = svc->heartbeat_timeout_ms;
    lc.purge_queue_on_restart = svc->purge_queue_on_restart;
    lc.on_start = service_on_start;
    lc.user = svc;

    svc->looper = ipc_looper_create(&lc);
    if (!svc->looper) return false;

    if (spawn_task) return ipc_looper_start(svc->looper);

    /* Khong co task -> khung tu goi moc doi, y het thu tu tren. */
    service_on_start(svc->looper, svc);
    return true;
}

bool app_service_restart(app_service_t *svc)
{
    if (!svc || !svc->looper) return false;
    return ipc_looper_restart_inplace(svc->looper);
}

int32_t app_service_get(const char *name, uint32_t key, int32_t def)
{
    app_service_t *svc = app_service_find(name);
    if (!svc || !svc->get) return SVC_VALUE_INVALID;
    return svc->get(svc, key, def);
}

bool app_service_set(const char *name, uint32_t key, int32_t value)
{
    app_service_t *svc = app_service_find(name);
    if (!svc || !svc->set) return false;
    return svc->set(svc, key, value);
}
