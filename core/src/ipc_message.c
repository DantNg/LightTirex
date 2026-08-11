#include "ipc_message.h"
#include <string.h>

#ifndef IPC_MESSAGE_POOL_SIZE
#define IPC_MESSAGE_POOL_SIZE 64
#endif

static ipc_message_t s_pool[IPC_MESSAGE_POOL_SIZE];
static ipc_message_t *s_free_list;
static uint32_t s_free_count;
static uint32_t s_low_watermark;
static bool s_inited;

void ipc_message_pool_init(void)
{
    if (s_inited) return;
    memset(s_pool, 0, sizeof(s_pool));
    s_free_list = NULL;
    for (int i = IPC_MESSAGE_POOL_SIZE - 1; i >= 0; --i) {
        s_pool[i].pool_index = (uint16_t)i;
        s_pool[i].next = s_free_list;
        s_free_list = &s_pool[i];
    }
    s_free_count = IPC_MESSAGE_POOL_SIZE;
    s_low_watermark = IPC_MESSAGE_POOL_SIZE;
    s_inited = true;
}

ipc_message_t *ipc_message_obtain(void)
{
    ipc_message_t *m = NULL;
    ipc_enter_critical();
    if (s_free_list) {
        m = s_free_list;
        s_free_list = m->next;
        s_free_count--;
        if (s_free_count < s_low_watermark) s_low_watermark = s_free_count;
    }
    ipc_exit_critical();
    if (!m) return NULL;

    uint16_t idx = m->pool_index;
    memset(m, 0, sizeof(*m));
    m->pool_index = idx;
    m->in_use = 1;
    m->prio = 128; /* mac dinh: uu tien thuong */
    return m;
}

void ipc_message_recycle(ipc_message_t *m)
{
    if (!m || !m->in_use) return;

    /* Giai phong payload TRUOC khi vao critical section. */
    if (m->payload_free && m->payload) {
        m->payload_free(m->payload);
        m->payload = NULL;
    }

    if (m->pool_index == IPC_MSG_NOT_POOLED) {
        m->in_use = 0;
        return; /* message do nguoi dung so huu: chi danh dau */
    }

    ipc_enter_critical();
    m->in_use = 0;
    m->target = NULL;
    m->runnable = NULL;
    m->owner_tag = 0;
    m->next = s_free_list;
    s_free_list = m;
    s_free_count++;
    ipc_exit_critical();
}

uint32_t ipc_message_reclaim_by_owner(uint32_t owner_tag)
{
    uint32_t n = 0;
    if (owner_tag == 0) return 0;
    for (int i = 0; i < IPC_MESSAGE_POOL_SIZE; ++i) {
        ipc_message_t *m = &s_pool[i];
        bool hit = false;
        ipc_enter_critical();
        hit = (m->in_use && m->owner_tag == owner_tag);
        ipc_exit_critical();
        if (hit) {
            ipc_message_recycle(m);
            n++;
        }
    }
    return n;
}

void ipc_message_pool_stats(uint32_t *free_count, uint32_t *low_watermark)
{
    ipc_enter_critical();
    if (free_count) *free_count = s_free_count;
    if (low_watermark) *low_watermark = s_low_watermark;
    ipc_exit_critical();
}
