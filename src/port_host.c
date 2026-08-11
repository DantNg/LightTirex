/*
 * port_host.c - hien thuc ipc_port.h tren desktop (pthreads).
 *
 * LSP: thay the hoan toan port_freertos.c. Cung mot code nghiep vu chay
 * duoc ca tren PC lan tren MCU, khong #ifdef rai rac trong core.
 *
 * Build voi -DIPC_PORT_HOST. Tren MinGW can winpthreads (mac dinh co).
 */
#if defined(IPC_PORT_HOST)

/*
 * PHAI dat truoc moi #include.
 *
 * Bien dich voi -std=c11 lam glibc bat __STRICT_ANSI__, khi do
 * PTHREAD_MUTEX_RECURSIVE, pthread_mutexattr_settype, nanosleep va
 * CLOCK_MONOTONIC deu KHONG duoc khai bao -> build tren Linux se hong.
 * MinGW khong kiem tra feature macro nen tren Windows khong lo ra.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "ipc_port.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------- time ---------------- */

static uint64_t mono_ms(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint64_t s_epoch;

ipc_tick_t ipc_now_ms(void)
{
    if (!s_epoch) s_epoch = mono_ms();
    return (ipc_tick_t)(mono_ms() - s_epoch);
}

void ipc_sleep_ms(uint32_t ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---------------- mutex ---------------- */

ipc_mutex_t ipc_mutex_create(void)
{
    pthread_mutex_t *m = malloc(sizeof(*m));
    if (!m) return NULL;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}

void ipc_mutex_destroy(ipc_mutex_t m)
{
    if (!m) return;
    pthread_mutex_destroy((pthread_mutex_t *)m);
    free(m);
}

bool ipc_mutex_lock(ipc_mutex_t m, ipc_tick_t timeout_ms)
{
    if (!m) return false;
    if (timeout_ms == IPC_WAIT_FOREVER)
        return pthread_mutex_lock((pthread_mutex_t *)m) == 0;

    /* Vong thu lai: pthread_mutex_timedlock khong co tren moi nen tang. */
    uint64_t deadline = mono_ms() + timeout_ms;
    for (;;) {
        if (pthread_mutex_trylock((pthread_mutex_t *)m) == 0) return true;
        if (mono_ms() >= deadline) return false;
        ipc_sleep_ms(1);
    }
}

void ipc_mutex_unlock(ipc_mutex_t m)
{
    if (m) pthread_mutex_unlock((pthread_mutex_t *)m);
}

/* ---------------- semaphore nhi phan ---------------- */

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    bool signaled;
} hsem_t;

ipc_sem_t ipc_sem_create(void)
{
    hsem_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    return s;
}

void ipc_sem_destroy(ipc_sem_t sem)
{
    hsem_t *s = sem;
    if (!s) return;
    pthread_cond_destroy(&s->c);
    pthread_mutex_destroy(&s->m);
    free(s);
}

bool ipc_sem_take(ipc_sem_t sem, ipc_tick_t timeout_ms)
{
    hsem_t *s = sem;
    if (!s) return false;
    pthread_mutex_lock(&s->m);
    bool got = false;
    if (timeout_ms == IPC_WAIT_FOREVER) {
        while (!s->signaled) pthread_cond_wait(&s->c, &s->m);
        got = true;
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeout_ms / 1000);
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (!s->signaled) {
            if (pthread_cond_timedwait(&s->c, &s->m, &ts) == ETIMEDOUT) break;
        }
        got = s->signaled;
    }
    if (got) s->signaled = false;
    pthread_mutex_unlock(&s->m);
    return got;
}

void ipc_sem_give(ipc_sem_t sem)
{
    hsem_t *s = sem;
    if (!s) return;
    pthread_mutex_lock(&s->m);
    s->signaled = true;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
}

void ipc_sem_give_from_isr(ipc_sem_t s, bool *higher_prio_woken)
{
    if (higher_prio_woken) *higher_prio_woken = false;
    ipc_sem_give(s);   /* desktop khong co ISR */
}

/* ---------------- task ---------------- */

typedef struct {
    pthread_t th;
    ipc_task_fn_t fn;
    void *arg;
    volatile bool alive;
    volatile bool detached;
} htask_t;

static pthread_key_t s_tls_key;
static pthread_key_t s_self_key;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

static void make_keys(void)
{
    pthread_key_create(&s_tls_key, NULL);
    pthread_key_create(&s_self_key, NULL);
}

static void *thread_trampoline(void *p)
{
    htask_t *t = p;
    pthread_once(&s_once, make_keys);
    pthread_setspecific(s_self_key, t);
    t->fn(t->arg);
    t->alive = false;
    return NULL;
}

bool ipc_task_create(ipc_task_t *out, const char *name, ipc_task_fn_t fn,
                     void *arg, uint32_t stack_words, uint8_t prio)
{
    (void)name; (void)stack_words; (void)prio;
    pthread_once(&s_once, make_keys);

    htask_t *t = calloc(1, sizeof(*t));
    if (!t) return false;
    t->fn = fn;
    t->arg = arg;
    t->alive = true;
    if (pthread_create(&t->th, NULL, thread_trampoline, t) != 0) {
        free(t);
        return false;
    }
    pthread_detach(t->th);
    if (out) *out = t;
    return true;
}

void ipc_task_delete(ipc_task_t task)
{
    htask_t *t = task;
    if (!t) {
        t = pthread_getspecific(s_self_key);
        if (!t) return;
    }
    t->alive = false;
    if (pthread_equal(t->th, pthread_self())) {
        pthread_exit(NULL);   /* tu ket thuc: giong vTaskDelete(NULL) */
    }
    /*
     * pthreads khong co "giet thread khac" an toan va di dong. Tren desktop
     * ta chi danh dau chet; code chay tren MCU moi thuc su bi xoa. Test
     * kich ban treo nen dung fake clock + poll thay vi thread that.
     */
}

bool ipc_task_is_alive(ipc_task_t t) { return t && ((htask_t *)t)->alive; }

ipc_task_t ipc_task_self(void)
{
    pthread_once(&s_once, make_keys);
    return pthread_getspecific(s_self_key);
}

void *ipc_tls_get(void)
{
    pthread_once(&s_once, make_keys);
    return pthread_getspecific(s_tls_key);
}

void ipc_tls_set(void *p)
{
    pthread_once(&s_once, make_keys);
    pthread_setspecific(s_tls_key, p);
}

/* ---------------- critical section ---------------- */

/* Khoi tao tre bang pthread_once: PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
 * khong co tren moi nen tang (MinGW, macOS). */
static pthread_mutex_t s_crit;
static pthread_once_t  s_crit_once = PTHREAD_ONCE_INIT;

static void make_crit(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_crit, &a);
    pthread_mutexattr_destroy(&a);
}

void ipc_enter_critical(void)
{
    pthread_once(&s_crit_once, make_crit);
    pthread_mutex_lock(&s_crit);
}

void ipc_exit_critical(void) { pthread_mutex_unlock(&s_crit); }
bool ipc_in_isr(void)         { return false; }

#endif /* IPC_PORT_HOST */
