#include "ipc_service.h"

#include <stdio.h>
#include <string.h>

#ifndef IPC_SERVICE_NAME_LEN
#define IPC_SERVICE_NAME_LEN 20
#endif

struct ipc_service {
    char           name[IPC_SERVICE_NAME_LEN];
    ipc_handler_t *handler;
    bool           in_use;
    bool           alive;
};

typedef struct {
    char name[IPC_SERVICE_NAME_LEN];
    ipc_death_recipient_fn fn;
    void *user;
    bool  in_use;
} death_slot_t;

static ipc_service_t s_services[IPC_MAX_SERVICES];
static death_slot_t  s_recipients[IPC_MAX_DEATH_RECIPIENTS];
static ipc_mutex_t   s_lock;

void ipc_service_manager_init(void)
{
    if (s_lock) return;
    memset(s_services, 0, sizeof(s_services));
    memset(s_recipients, 0, sizeof(s_recipients));
    s_lock = ipc_mutex_create();
}

static ipc_service_t *find_locked(const char *name)
{
    for (int i = 0; i < IPC_MAX_SERVICES; ++i) {
        if (s_services[i].in_use && strncmp(s_services[i].name, name,
                                            IPC_SERVICE_NAME_LEN) == 0)
            return &s_services[i];
    }
    return NULL;
}

bool ipc_service_register(const char *name, ipc_handler_t *h)
{
    if (!name || !h) return false;
    ipc_service_manager_init();
    if (!ipc_mutex_lock(s_lock, 1000)) return false;

    ipc_service_t *s = find_locked(name);
    if (!s) {
        for (int i = 0; i < IPC_MAX_SERVICES; ++i) {
            if (!s_services[i].in_use) { s = &s_services[i]; break; }
        }
    }
    if (!s) { ipc_mutex_unlock(s_lock); return false; }

    snprintf(s->name, sizeof(s->name), "%s", name);
    s->handler = h;
    s->in_use = true;
    s->alive = true;
    ipc_mutex_unlock(s_lock);
    return true;
}

void ipc_service_unregister(const char *name)
{
    if (!name || !s_lock) return;
    if (!ipc_mutex_lock(s_lock, 1000)) return;
    ipc_service_t *s = find_locked(name);
    if (s) memset(s, 0, sizeof(*s));
    ipc_mutex_unlock(s_lock);
}

ipc_handler_t *ipc_service_get(const char *name)
{
    if (!name || !s_lock) return NULL;
    ipc_handler_t *h = NULL;
    if (!ipc_mutex_lock(s_lock, 1000)) return NULL;
    ipc_service_t *s = find_locked(name);
    if (s && s->alive) h = s->handler;
    ipc_mutex_unlock(s_lock);
    return h;
}

bool ipc_service_send(const char *name, uint32_t what, int32_t arg1, int32_t arg2)
{
    ipc_handler_t *h = ipc_service_get(name);
    if (!h) return false;
    ipc_message_t *m = ipc_message_obtain();
    if (!m) return false;
    m->what = what;
    m->arg1 = arg1;
    m->arg2 = arg2;
    return ipc_handler_send(h, m);
}

bool ipc_service_send_msg(const char *name, ipc_message_t *msg)
{
    ipc_handler_t *h = ipc_service_get(name);
    if (!h) { ipc_message_recycle(msg); return false; }
    return ipc_handler_send(h, msg);
}

bool ipc_service_call_sync(const char *name, uint32_t what, int32_t arg1,
                           void *payload, uint32_t timeout_ms)
{
    ipc_handler_t *h = ipc_service_get(name);
    if (!h) return false;
    return ipc_handler_send_sync(h, what, arg1, payload, timeout_ms);
}

bool ipc_service_link_to_death(const char *name, ipc_death_recipient_fn fn, void *user)
{
    if (!name || !fn) return false;
    ipc_service_manager_init();
    if (!ipc_mutex_lock(s_lock, 1000)) return false;
    bool ok = false;
    for (int i = 0; i < IPC_MAX_DEATH_RECIPIENTS; ++i) {
        if (!s_recipients[i].in_use) {
            snprintf(s_recipients[i].name, sizeof(s_recipients[i].name), "%s", name);
            s_recipients[i].fn = fn;
            s_recipients[i].user = user;
            s_recipients[i].in_use = true;
            ok = true;
            break;
        }
    }
    ipc_mutex_unlock(s_lock);
    return ok;
}

void ipc_service_unlink_to_death(const char *name, ipc_death_recipient_fn fn, void *user)
{
    if (!s_lock) return;
    if (!ipc_mutex_lock(s_lock, 1000)) return;
    for (int i = 0; i < IPC_MAX_DEATH_RECIPIENTS; ++i) {
        death_slot_t *d = &s_recipients[i];
        if (d->in_use && d->fn == fn && d->user == user &&
            strncmp(d->name, name, IPC_SERVICE_NAME_LEN) == 0) {
            memset(d, 0, sizeof(*d));
        }
    }
    ipc_mutex_unlock(s_lock);
}

void ipc_service_notify_state(ipc_looper_t *lp, bool alive)
{
    if (!lp || !s_lock) return;
    const char *lname = ipc_looper_name(lp);
    uint32_t gen = ipc_looper_generation(lp);

    /* Chup danh sach nguoi nhan roi moi goi callback ngoai vung khoa. */
    ipc_death_recipient_fn fns[IPC_MAX_DEATH_RECIPIENTS];
    void *users[IPC_MAX_DEATH_RECIPIENTS];
    char  names[IPC_MAX_DEATH_RECIPIENTS][IPC_SERVICE_NAME_LEN];
    int   n = 0;

    if (!ipc_mutex_lock(s_lock, 1000)) return;
    for (int i = 0; i < IPC_MAX_SERVICES; ++i) {
        ipc_service_t *s = &s_services[i];
        if (!s->in_use || !s->handler) continue;
        if (s->handler->looper != lp) continue;
        s->alive = alive;

        for (int j = 0; j < IPC_MAX_DEATH_RECIPIENTS && n < IPC_MAX_DEATH_RECIPIENTS; ++j) {
            death_slot_t *d = &s_recipients[j];
            if (d->in_use && strncmp(d->name, s->name, IPC_SERVICE_NAME_LEN) == 0) {
                fns[n] = d->fn;
                users[n] = d->user;
                snprintf(names[n], IPC_SERVICE_NAME_LEN, "%s", s->name);
                n++;
            }
        }
    }
    ipc_mutex_unlock(s_lock);

    (void)lname;
    for (int i = 0; i < n; ++i) fns[i](names[i], alive, gen, users[i]);
}
