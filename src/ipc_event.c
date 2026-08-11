#include "ipc_event.h"
#include "ipc_service.h"

#include <stdio.h>
#include <string.h>

#define BUS_LOCK_MS 1000u

typedef struct {
    uint32_t topic;
    uint32_t what;
    ipc_handler_t *handler;                 /* mot trong hai duoc dung */
    char service[IPC_BUS_NAME_LEN];
    bool by_name;
    bool in_use;
    uint16_t gen;
    uint16_t slot;
} sub_t;

typedef struct {
    uint32_t topic;
    ipc_event_t ev;
    bool in_use;
} retained_t;

static sub_t      s_subs[IPC_MAX_SUBSCRIPTIONS];
static retained_t s_retained[IPC_MAX_RETAINED_TOPICS];
static ipc_mutex_t s_lock;
static ipc_bus_stats_t s_stats;

void ipc_bus_init(void)
{
    if (s_lock) return;
    ipc_message_pool_init();
    s_lock = ipc_mutex_create();
}

static ipc_sub_id_t make_id(const sub_t *s)
{
    return ((ipc_sub_id_t)s->gen << 16) | (ipc_sub_id_t)(s->slot + 1);
}

static sub_t *resolve(ipc_sub_id_t id)
{
    if (id == IPC_SUB_NONE) return NULL;
    uint16_t slot = (uint16_t)((id & 0xFFFFu) - 1);
    uint16_t gen  = (uint16_t)(id >> 16);
    if (slot >= IPC_MAX_SUBSCRIPTIONS) return NULL;
    sub_t *s = &s_subs[slot];
    return (s->in_use && s->gen == gen) ? s : NULL;
}

static sub_t *alloc_sub(uint32_t topic, uint32_t what)
{
    for (int i = 0; i < IPC_MAX_SUBSCRIPTIONS; ++i) {
        if (!s_subs[i].in_use) {
            sub_t *s = &s_subs[i];
            uint16_t gen = (uint16_t)(s->gen + 1);
            memset(s, 0, sizeof(*s));
            s->slot = (uint16_t)i;
            s->gen = gen;
            s->topic = topic;
            s->what = what;
            s->in_use = true;
            return s;
        }
    }
    return NULL;
}

/* Giao mot su kien cho mot nguoi nghe. Chay ngoai vung khoa bus. */
static bool deliver(const sub_t *s, const ipc_event_t *ev)
{
    /* Phan giai theo ten tai DUNG thoi diem cong bo: dich vu vua duoc hoi
     * sinh se co handler moi, va no van nhan duoc su kien nay. */
    ipc_handler_t *h = s->by_name ? ipc_service_get(s->service) : s->handler;
    if (!h) return false;

    ipc_message_t *m = ipc_message_obtain();
    if (!m) return false;
    m->what = s->what;
    m->topic = ev->topic;
    m->arg1 = ev->arg1;
    m->arg2 = ev->arg2;
    m->payload = ev->payload;
    m->payload_len = ev->payload_len;
    /* payload_free co tinh de NULL: bus khong so huu payload. */
    return ipc_handler_send(h, m);
}

/* ------------------------------------------------------------------ */

ipc_sub_id_t ipc_bus_subscribe(uint32_t topic, ipc_handler_t *h, uint32_t what)
{
    if (!h) return IPC_SUB_NONE;
    ipc_bus_init();
    if (!ipc_mutex_lock(s_lock, BUS_LOCK_MS)) return IPC_SUB_NONE;

    sub_t *s = alloc_sub(topic, what);
    if (s) s->handler = h;
    ipc_sub_id_t id = s ? make_id(s) : IPC_SUB_NONE;

    /* Chup gia tri giu lai de giao NGOAI vung khoa. */
    ipc_event_t snap;
    bool have_retained = false;
    if (s) {
        for (int i = 0; i < IPC_MAX_RETAINED_TOPICS; ++i) {
            if (s_retained[i].in_use && s_retained[i].topic == topic) {
                snap = s_retained[i].ev;
                have_retained = true;
                break;
            }
        }
    }
    sub_t copy;
    if (have_retained) copy = *s;
    ipc_mutex_unlock(s_lock);

    /* Nguoi dang ky muon nhan duoc trang thai hien tai ngay, khong phai cho
     * den nhip cap nhat sau. */
    if (have_retained && deliver(&copy, &snap)) s_stats.delivered++;
    return id;
}

ipc_sub_id_t ipc_bus_subscribe_service(uint32_t topic, const char *service,
                                       uint32_t what)
{
    if (!service) return IPC_SUB_NONE;
    ipc_bus_init();
    if (!ipc_mutex_lock(s_lock, BUS_LOCK_MS)) return IPC_SUB_NONE;

    sub_t *s = alloc_sub(topic, what);
    if (s) {
        s->by_name = true;
        snprintf(s->service, sizeof(s->service), "%s", service);
    }
    ipc_sub_id_t id = s ? make_id(s) : IPC_SUB_NONE;

    ipc_event_t snap;
    bool have_retained = false;
    sub_t copy;
    if (s) {
        for (int i = 0; i < IPC_MAX_RETAINED_TOPICS; ++i) {
            if (s_retained[i].in_use && s_retained[i].topic == topic) {
                snap = s_retained[i].ev;
                have_retained = true;
                copy = *s;
                break;
            }
        }
    }
    ipc_mutex_unlock(s_lock);

    if (have_retained && deliver(&copy, &snap)) s_stats.delivered++;
    return id;
}

void ipc_bus_unsubscribe(ipc_sub_id_t id)
{
    if (!s_lock || !ipc_mutex_lock(s_lock, BUS_LOCK_MS)) return;
    sub_t *s = resolve(id);
    if (s) s->in_use = false;
    ipc_mutex_unlock(s_lock);
}

uint32_t ipc_bus_unsubscribe_handler(ipc_handler_t *h)
{
    if (!h || !s_lock || !ipc_mutex_lock(s_lock, BUS_LOCK_MS)) return 0;
    uint32_t n = 0;
    for (int i = 0; i < IPC_MAX_SUBSCRIPTIONS; ++i) {
        if (s_subs[i].in_use && !s_subs[i].by_name && s_subs[i].handler == h) {
            s_subs[i].in_use = false;
            n++;
        }
    }
    ipc_mutex_unlock(s_lock);
    return n;
}

uint32_t ipc_bus_unsubscribe_service(const char *service)
{
    if (!service || !s_lock || !ipc_mutex_lock(s_lock, BUS_LOCK_MS)) return 0;
    uint32_t n = 0;
    for (int i = 0; i < IPC_MAX_SUBSCRIPTIONS; ++i) {
        if (s_subs[i].in_use && s_subs[i].by_name &&
            strncmp(s_subs[i].service, service, IPC_BUS_NAME_LEN) == 0) {
            s_subs[i].in_use = false;
            n++;
        }
    }
    ipc_mutex_unlock(s_lock);
    return n;
}

/* ------------------------------------------------------------------ */

uint32_t ipc_bus_publish_ev(const ipc_event_t *ev)
{
    if (!ev || !s_lock) return 0;
    if (!ipc_mutex_lock(s_lock, BUS_LOCK_MS)) return 0;

    /* Chup danh sach nguoi nghe roi tha khoa: giao su kien co the cham vao
     * mutex cua looper khac, khong duoc giu khoa bus trong luc do. */
    sub_t snap[IPC_MAX_SUBSCRIPTIONS];
    uint32_t n = 0;
    for (int i = 0; i < IPC_MAX_SUBSCRIPTIONS; ++i) {
        if (s_subs[i].in_use && s_subs[i].topic == ev->topic)
            snap[n++] = s_subs[i];
    }
    s_stats.published++;
    ipc_mutex_unlock(s_lock);

    uint32_t delivered = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (deliver(&snap[i], ev)) delivered++;
        else s_stats.dropped++;
    }
    s_stats.delivered += delivered;
    return delivered;
}

uint32_t ipc_bus_publish(uint32_t topic, int32_t arg1, int32_t arg2)
{
    ipc_event_t ev = { .topic = topic, .arg1 = arg1, .arg2 = arg2,
                       .payload = NULL, .payload_len = 0 };
    return ipc_bus_publish_ev(&ev);
}

uint32_t ipc_bus_publish_retained(uint32_t topic, int32_t arg1, int32_t arg2)
{
    ipc_bus_init();
    ipc_event_t ev = { .topic = topic, .arg1 = arg1, .arg2 = arg2,
                       .payload = NULL, .payload_len = 0 };

    if (ipc_mutex_lock(s_lock, BUS_LOCK_MS)) {
        retained_t *slot = NULL;
        for (int i = 0; i < IPC_MAX_RETAINED_TOPICS; ++i) {
            if (s_retained[i].in_use && s_retained[i].topic == topic) { slot = &s_retained[i]; break; }
            if (!s_retained[i].in_use && !slot) slot = &s_retained[i];
        }
        if (slot) {
            slot->topic = topic;
            slot->ev = ev;
            slot->in_use = true;
        }
        ipc_mutex_unlock(s_lock);
    }
    return ipc_bus_publish_ev(&ev);
}

bool ipc_bus_get_retained(uint32_t topic, ipc_event_t *out)
{
    if (!out || !s_lock || !ipc_mutex_lock(s_lock, BUS_LOCK_MS)) return false;
    bool found = false;
    for (int i = 0; i < IPC_MAX_RETAINED_TOPICS; ++i) {
        if (s_retained[i].in_use && s_retained[i].topic == topic) {
            *out = s_retained[i].ev;
            found = true;
            break;
        }
    }
    ipc_mutex_unlock(s_lock);
    return found;
}

void ipc_bus_clear_retained(uint32_t topic)
{
    if (!s_lock || !ipc_mutex_lock(s_lock, BUS_LOCK_MS)) return;
    for (int i = 0; i < IPC_MAX_RETAINED_TOPICS; ++i)
        if (s_retained[i].in_use && s_retained[i].topic == topic)
            s_retained[i].in_use = false;
    ipc_mutex_unlock(s_lock);
}

uint32_t ipc_bus_publish_from_isr(uint32_t topic, int32_t arg1, int32_t arg2,
                                  bool *higher_prio_woken)
{
    uint32_t delivered = 0;
    /* Tu ISR: khong mutex, khong tra cuu theo ten (ServiceManager dung mutex). */
    for (int i = 0; i < IPC_MAX_SUBSCRIPTIONS; ++i) {
        sub_t *s = &s_subs[i];
        if (!s->in_use || s->topic != topic || s->by_name || !s->handler) continue;

        ipc_message_t *m = ipc_message_obtain();
        if (!m) continue;
        m->what = s->what;
        m->topic = topic;
        m->arg1 = arg1;
        m->arg2 = arg2;
        if (ipc_handler_send_from_isr(s->handler, m, higher_prio_woken)) delivered++;
        else ipc_message_recycle(m);
    }
    return delivered;
}

uint32_t ipc_bus_subscriber_count(uint32_t topic)
{
    if (!s_lock || !ipc_mutex_lock(s_lock, BUS_LOCK_MS)) return 0;
    uint32_t n = 0;
    for (int i = 0; i < IPC_MAX_SUBSCRIPTIONS; ++i)
        if (s_subs[i].in_use && s_subs[i].topic == topic) n++;
    ipc_mutex_unlock(s_lock);
    return n;
}

void ipc_bus_get_stats(ipc_bus_stats_t *out) { if (out) *out = s_stats; }

void ipc_bus_dump(void (*print)(const char *line))
{
    if (!print || !s_lock || !ipc_mutex_lock(s_lock, BUS_LOCK_MS)) return;
    char line[128];
    for (int i = 0; i < IPC_MAX_SUBSCRIPTIONS; ++i) {
        sub_t *s = &s_subs[i];
        if (!s->in_use) continue;
        snprintf(line, sizeof(line), "sub[%2d] topic=%u -> %s what=%u", i,
                 (unsigned)s->topic,
                 s->by_name ? s->service : (s->handler && s->handler->name
                                                ? s->handler->name : "?"),
                 (unsigned)s->what);
        print(line);
    }
    snprintf(line, sizeof(line), "bus: pub=%u delivered=%u dropped=%u",
             (unsigned)s_stats.published, (unsigned)s_stats.delivered,
             (unsigned)s_stats.dropped);
    print(line);
    ipc_mutex_unlock(s_lock);
}
