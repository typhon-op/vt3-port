/*
 * The runtime log: a bounded in-memory ring drained by one writer thread.
 *
 * Producers (render, audio callback, platform, installer, the guest's own threads) only format
 * into a local buffer and copy it into the ring, so they never touch a file and never wait on
 * I/O. A single writer thread does the I/O: on Android it hands each entry to the app's
 * DiagLog.nativeWrite (which appends to the rotating diagnostic file and logcat), and on the host
 * it writes to stderr. If the ring is full the newest entry is dropped and counted: logging can
 * lose detail under pressure, but it can never stall a frame or a callback.
 */
#include "vt3/vt3_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __ANDROID__
#include <android/log.h>
#include "vt3/vt3_diag.h"
#endif

#define RING_SLOTS   512
#define COMP_BYTES   24
#define MSG_BYTES    512

typedef struct log_slot {
    vt3_log_level level;
    int code;
    char component[COMP_BYTES];
    char message[MSG_BYTES];
} log_slot;

static log_slot g_ring[RING_SLOTS];
static _Atomic unsigned g_head;      /* producer index */
static _Atomic unsigned g_tail;      /* writer index */
static _Atomic unsigned g_dropped;
static _Atomic int g_detail;
static _Atomic int g_stop;
static _Atomic int g_started;
static pthread_mutex_t g_push_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_drained = PTHREAD_COND_INITIALIZER;
static vt3_log_sink g_sink;
static void *g_sink_user;

static char *copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
    return dst;
}

void vt3_log_set_detail(int on) { atomic_store(&g_detail, on ? 1 : 0); }
int vt3_log_detail(void) { return atomic_load(&g_detail); }

const char *vt3_log_level_name(vt3_log_level level)
{
    switch (level) {
    case VT3_LOG_DEBUG: return "DEBUG";
    case VT3_LOG_INFO:  return "INFO";
    case VT3_LOG_WARN:  return "WARN";
    case VT3_LOG_ERROR: return "ERROR";
    case VT3_LOG_FATAL: return "FATAL";
    default:            return "INFO";
    }
}

int vt3_log_dropped(void) { return (int)atomic_load(&g_dropped); }

void vt3_log_set_sink(vt3_log_sink sink, void *user)
{
    g_sink = sink;
    g_sink_user = user;
}

#ifndef __ANDROID__
/* The host sink: one line on stderr, the same shape the app's file has. */
static void stderr_sink(vt3_log_level level, const char *component, int code,
                        const char *message, void *user)
{
    (void)user;
    if (code)
        fprintf(stderr, "[vt3:%s] %s (%d): %s\n", vt3_log_level_name(level), component, code, message);
    else
        fprintf(stderr, "[vt3:%s] %s: %s\n", vt3_log_level_name(level), component, message);
    fflush(stderr);
}
#endif

#ifdef __ANDROID__
/* Logcat is the developer's view; it costs one write per entry on the writer thread only. */
static void logcat_sink(vt3_log_level level, const char *component, int code,
                        const char *message, void *user)
{
    (void)user;
    static const int prio[] = { ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN,
                                ANDROID_LOG_ERROR, ANDROID_LOG_FATAL };
    if (code)
        __android_log_print(prio[level], "vt3", "%s (%d): %s", component, code, message);
    else
        __android_log_print(prio[level], "vt3", "%s: %s", component, message);
    /* The app's rotating file (and the crash/last-error preservation) lives on the Java side. */
    vt3_diag_jni_write((int)level, component, code, message);
}
#endif

static void deliver(const log_slot *s)
{
    if (g_sink) {
        g_sink(s->level, s->component, s->code, s->message, g_sink_user);
        return;
    }
#ifdef __ANDROID__
    logcat_sink(s->level, s->component, s->code, s->message, NULL);
#else
    stderr_sink(s->level, s->component, s->code, s->message, NULL);
#endif
}

static void *writer_thread(void *arg)
{
    (void)arg;
    struct timespec nap = { 0, 2000000 };   /* 2 ms */
    while (!atomic_load(&g_stop)) {
        unsigned head = atomic_load(&g_head), tail = atomic_load(&g_tail);
        if (head == tail) {
            nanosleep(&nap, NULL);
            continue;
        }
        log_slot copy = g_ring[tail % RING_SLOTS];
        /* Deliver before the slot is released, so "head == tail" really means "written". */
        deliver(&copy);
        atomic_store(&g_tail, tail + 1);
        if (atomic_load(&g_head) == atomic_load(&g_tail)) {
            pthread_mutex_lock(&g_push_mu);
            pthread_cond_broadcast(&g_drained);
            pthread_mutex_unlock(&g_push_mu);
        }
    }
    return NULL;
}

static void start_writer(void)
{
    if (atomic_exchange(&g_started, 1)) return;
    pthread_t t;
    if (pthread_create(&t, NULL, writer_thread, NULL) == 0) pthread_detach(t);
    const char *detail = getenv("VT3_LOG_DETAIL");
    if (detail && *detail && strcmp(detail, "0") != 0) vt3_log_set_detail(1);
}

void vt3_logc(const char *component, vt3_log_level level, int code, const char *fmt, ...)
{
    /* DEBUG is the detailed mode's extra detail; dropping it here keeps even the ring untouched. */
    if (level == VT3_LOG_DEBUG && !atomic_load(&g_detail)) return;

    log_slot s;
    memset(&s, 0, sizeof s);
    s.level = level;
    s.code = code;
    copy_str(s.component, sizeof s.component, component ? component : "runtime");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s.message, sizeof s.message, fmt, ap);
    va_end(ap);

    start_writer();
    /* The head is read and advanced under the lock, so two producers cannot claim one slot. */
    pthread_mutex_lock(&g_push_mu);
    unsigned head = atomic_load(&g_head);
    unsigned tail = atomic_load(&g_tail);
    if (head - tail >= RING_SLOTS) {
        pthread_mutex_unlock(&g_push_mu);
        atomic_fetch_add(&g_dropped, 1);
        if (level != VT3_LOG_FATAL) return;
        int room = 0;
        /* A fatal entry must not be the one that goes missing: wait for room, briefly. */
        struct timespec nap = { 0, 1000000 };
        for (int i = 0; i < 50 && !room; i++) {
            nanosleep(&nap, NULL);
            pthread_mutex_lock(&g_push_mu);
            head = atomic_load(&g_head);
            tail = atomic_load(&g_tail);
            room = head - tail < RING_SLOTS;
            if (!room) pthread_mutex_unlock(&g_push_mu);
        }
        /* room == 1 leaves the lock held for the slot copy below. */
        if (!room) return;
    }
    g_ring[head % RING_SLOTS] = s;
    atomic_store(&g_head, head + 1);
    pthread_mutex_unlock(&g_push_mu);
}

void vt3_log(vt3_log_level level, const char *fmt, ...)
{
    if (level == VT3_LOG_DEBUG && !atomic_load(&g_detail)) return;
    log_slot s;
    memset(&s, 0, sizeof s);
    s.level = level;
    copy_str(s.component, sizeof s.component, "runtime");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s.message, sizeof s.message, fmt, ap);
    va_end(ap);

    start_writer();
    pthread_mutex_lock(&g_push_mu);
    unsigned head = atomic_load(&g_head);
    if (head - atomic_load(&g_tail) >= RING_SLOTS) {
        pthread_mutex_unlock(&g_push_mu);
        atomic_fetch_add(&g_dropped, 1);
        return;
    }
    g_ring[head % RING_SLOTS] = s;
    atomic_store(&g_head, head + 1);
    pthread_mutex_unlock(&g_push_mu);
}

void vt3_log_flush(void)
{
    if (!atomic_load(&g_started)) return;
    struct timespec until, now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    until = now;
    until.tv_nsec += 300000000;
    if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&g_push_mu);
    while (atomic_load(&g_head) != atomic_load(&g_tail)) {
        struct timespec t = { 0, 2000000 };
        pthread_cond_timedwait(&g_drained, &g_push_mu, &t);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > until.tv_sec || (now.tv_sec == until.tv_sec && now.tv_nsec >= until.tv_nsec)) break;
    }
    pthread_mutex_unlock(&g_push_mu);
}
