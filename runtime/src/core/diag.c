/*
 * Native diagnostics: frame statistics, lifecycle counters, GPU identity, audio state and the
 * crash line that survives the process. Everything is written to one atomically replaced file
 * (<dir>/diag_metrics.txt) so the Java report can read a consistent snapshot.
 *
 * Cost per frame is a handful of atomic increments; the file work happens on the platform thread,
 * at most once a second, and only when the app asked for diagnostics.
 */
#include "vt3/vt3_diag.h"

#include "vt3/vt3_audio.h"
#include "vt3/vt3_log.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define COUNTERS 16
#define HIST_BUCKETS 128

typedef struct diag_counter {
    char name[32];
    _Atomic long long value;
} diag_counter;

static struct {
    char dir[256];
    char version[32];
    _Atomic int active;
    _Atomic long long frames;
    _Atomic unsigned hist[HIST_BUCKETS];
    _Atomic long long max_ns;
    _Atomic int render_w, render_h, window_w, window_h;
    _Atomic int gpu_vendor;
    _Atomic unsigned gpu_api;
    _Atomic int vulkan_errors;
    _Atomic long long frames_at_tick;
    _Atomic unsigned last_event;
    char gpu_name[128];
    char gpu_ext[512];
    pthread_mutex_t state_mu;
    diag_counter counters[COUNTERS];
    int ncounters;
    long long started_ms;
    long long last_tick_ms;
    int crash_fd;
    _Atomic(vt3_audio *) audio;
} g;

/* A backend that does not report stream state (the host test stub) still links: weak default. */
__attribute__((weak)) int vt3_audio_info_get(const vt3_audio *a, vt3_audio_info *out)
{
    (void)a;
    (void)out;
    return 0;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- crash preservation ------------------------------------------------------ */

/* write(2) is warn_unused_result on Android; a dropped crash line must not abort the handler. */
static void write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
    }
}

static void crash_handler(int sig)
{
    if (g.crash_fd >= 0) {
        const char *name = "signal";
        switch (sig) {
        case SIGSEGV: name = "SIGSEGV (segmentation fault)"; break;
        case SIGABRT: name = "SIGABRT (abort)"; break;
        case SIGBUS:  name = "SIGBUS (bus error)"; break;
        case SIGFPE:  name = "SIGFPE (arithmetic fault)"; break;
        case SIGILL:  name = "SIGILL (illegal instruction)"; break;
        default: break;
        }
        /* write(2) only: async-signal-safe, unlike the logger and its ring. */
        static const char head[] = "\nFATAL native crash: ";
        write_all(g.crash_fd, head, sizeof head - 1);
        write_all(g.crash_fd, name, strlen(name));
        write_all(g.crash_fd, "\n", 1);
    }
    /* Let the platform's own crash collection (debuggerd/tombstone) run and the process die. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handlers(void)
{
    int fd = -1;
    if (g.dir[0]) {
        char path[512];
        snprintf(path, sizeof path, "%s/native_crash.txt", g.dir);
        /* Append: the previous session's crash stays above this session's start line. */
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    }
    g.crash_fd = fd;
    if (fd >= 0) {
        static const char line[] = "\n--- session start ---\n";
        write_all(fd, line, sizeof line - 1);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}

/* ---- frame statistics -------------------------------------------------------- */

void vt3_diag_frame(uint64_t dt_ns)
{
    if (!atomic_load(&g.active) || !dt_ns) return;
    atomic_fetch_add(&g.frames, 1);
    unsigned ms = (unsigned)(dt_ns / 1000000ull);
    if (ms >= HIST_BUCKETS) ms = HIST_BUCKETS - 1;
    atomic_fetch_add(&g.hist[ms], 1);
    long long prev = atomic_load(&g.max_ns);
    if ((long long)dt_ns > prev && !atomic_compare_exchange_strong(&g.max_ns, &prev, (long long)dt_ns))
        atomic_store(&g.max_ns, (long long)dt_ns);
}

static unsigned hist_pct(int pct)
{
    unsigned hist[HIST_BUCKETS];
    unsigned long long total = 0, want, acc = 0;
    for (int i = 0; i < HIST_BUCKETS; i++) {
        hist[i] = atomic_load(&g.hist[i]);
        total += hist[i];
    }
    if (!total) return 0;
    want = (total * (unsigned long long)pct + 99) / 100;
    for (int i = 0; i < HIST_BUCKETS; i++) {
        acc += hist[i];
        if (acc >= want) return (unsigned)i;
    }
    return HIST_BUCKETS - 1;
}

void vt3_diag_render_size(int w, int h)
{
    atomic_store(&g.render_w, w);
    atomic_store(&g.render_h, h);
}

void vt3_diag_window_size(int w, int h)
{
    atomic_store(&g.window_w, w);
    atomic_store(&g.window_h, h);
}

void vt3_diag_gpu(uint32_t vendor_id, const char *name, uint32_t api_version, const char *extensions)
{
    if (!atomic_load(&g.active)) return;
    atomic_store(&g.gpu_vendor, (int)vendor_id);
    atomic_store(&g.gpu_api, api_version);
    pthread_mutex_lock(&g.state_mu);
    snprintf(g.gpu_name, sizeof g.gpu_name, "%s", name ? name : "unknown");
    snprintf(g.gpu_ext, sizeof g.gpu_ext, "%s", extensions ? extensions : "");
    pthread_mutex_unlock(&g.state_mu);
}

void vt3_diag_vulkan_error(void)
{
    atomic_fetch_add(&g.vulkan_errors, 1);
}

/* ---- named counters ---------------------------------------------------------- */

static diag_counter *find_counter(const char *name)
{
    pthread_mutex_lock(&g.state_mu);
    diag_counter *slot = NULL;
    for (int i = 0; i < g.ncounters; i++) {
        if (strcmp(g.counters[i].name, name) == 0) { slot = &g.counters[i]; break; }
    }
    if (!slot && g.ncounters < COUNTERS) {
        slot = &g.counters[g.ncounters++];
        snprintf(slot->name, sizeof slot->name, "%s", name);
        atomic_store(&slot->value, 0);
    }
    pthread_mutex_unlock(&g.state_mu);
    return slot;
}

void vt3_diag_counter(const char *name, int delta)
{
    if (!atomic_load(&g.active) || !name) return;
    diag_counter *c = find_counter(name);
    if (c) atomic_fetch_add(&c->value, (long long)delta);
}

void vt3_diag_event(const char *name)
{
    if (!atomic_load(&g.active) || !name) return;
    diag_counter *c = find_counter(name);
    if (c) {
        atomic_fetch_add(&c->value, 1);
        /* A tiny intern table would be nicer; an offset is enough to tell reports apart. */
        atomic_fetch_add(&g.last_event, 1);
    }
}

void vt3_diag_bind_audio(vt3_audio *a) { atomic_store(&g.audio, a); }

void vt3_diag_audio(const vt3_audio *a)
{
    if (!atomic_load(&g.active) || !a) return;
    vt3_audio_info info;
    if (!vt3_audio_info_get(a, &info)) return;
    diag_counter *c;
    c = find_counter("audio_underruns");        if (c) atomic_store(&c->value, (long long)info.underruns);
    c = find_counter("audio_starved");          if (c) atomic_store(&c->value, (long long)info.starved);
    c = find_counter("audio_reopen_failures");  if (c) atomic_store(&c->value, (long long)info.reopen_failures);
    c = find_counter("audio_streaming");        if (c) atomic_store(&c->value, info.streaming);
    c = find_counter("audio_paused");           if (c) atomic_store(&c->value, info.paused);
    c = find_counter("audio_open");             if (c) atomic_store(&c->value, info.open);
    c = find_counter("audio_rate");             if (c) atomic_store(&c->value, (long long)info.sample_rate);
    c = find_counter("audio_channels");         if (c) atomic_store(&c->value, info.channels);
}

/* ---- snapshot ---------------------------------------------------------------- */

static const char *vendor_name(int id)
{
    switch ((uint32_t)id) {
    case 0x13B5: return "ARM";
    case 0x5143: return "Qualcomm";
    case 0x10DE: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    case 0x1010: return "Imagination";
    case 0x14D2: return "VeriSilicon";
    default: return "unknown";
    }
}

static void write_snapshot(long long now)
{
    if (!g.dir[0]) return;
    char path[512], tmp[512];
    snprintf(path, sizeof path, "%s/diag_metrics.txt", g.dir);
    snprintf(tmp, sizeof tmp, "%s/diag_metrics.txt.tmp", g.dir);

    long long frames = atomic_load(&g.frames);
    long long since_tick = frames - atomic_load(&g.frames_at_tick);
    long long tick_ms = g.last_tick_ms ? now - g.last_tick_ms : 0;
    double fps_now = tick_ms > 0 ? (double)since_tick * 1000.0 / (double)tick_ms : 0.0;
    long long uptime = now - g.started_ms;
    double fps_avg = uptime > 0 ? (double)frames * 1000.0 / (double)uptime : 0.0;
    atomic_store(&g.frames_at_tick, frames);
    g.last_tick_ms = now;

    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "version=%s\n", g.version);
    fprintf(f, "uptime_s=%lld\n", uptime / 1000);
    fprintf(f, "frames=%lld\n", frames);
    fprintf(f, "fps_now=%.1f\n", fps_now);
    fprintf(f, "fps_avg=%.1f\n", fps_avg);
    fprintf(f, "frame_p50_ms=%u\n", hist_pct(50));
    fprintf(f, "frame_p95_ms=%u\n", hist_pct(95));
    fprintf(f, "frame_max_ms=%.1f\n", (double)atomic_load(&g.max_ns) / 1e6);
    fprintf(f, "render_w=%d\nrender_h=%d\n", atomic_load(&g.render_w), atomic_load(&g.render_h));
    fprintf(f, "window_w=%d\nwindow_h=%d\n", atomic_load(&g.window_w), atomic_load(&g.window_h));
    fprintf(f, "vulkan_errors=%d\n", atomic_load(&g.vulkan_errors));
    fprintf(f, "log_dropped=%d\n", vt3_log_dropped());
    pthread_mutex_lock(&g.state_mu);
    fprintf(f, "gpu_vendor=%s (0x%04x)\n", vendor_name(atomic_load(&g.gpu_vendor)),
            (unsigned)atomic_load(&g.gpu_vendor));
    fprintf(f, "gpu_name=%s\n", g.gpu_name);
    fprintf(f, "gpu_api=%u.%u\n", (atomic_load(&g.gpu_api) >> 22) & 0x7f,
            (atomic_load(&g.gpu_api) >> 12) & 0x3ff);
    fprintf(f, "gpu_extensions=%s\n", g.gpu_ext);
    for (int i = 0; i < g.ncounters; i++)
        fprintf(f, "count_%s=%lld\n", g.counters[i].name, atomic_load(&g.counters[i].value));
    pthread_mutex_unlock(&g.state_mu);
    if (fclose(f) != 0) return;
    rename(tmp, path);   /* the reader always sees a complete file */
}

/* ---- lifecycle --------------------------------------------------------------- */

void vt3_diag_start(const char *dir, const char *version)
{
    pthread_mutex_init(&g.state_mu, NULL);
    g.started_ms = now_ms();
    g.last_tick_ms = 0;
    g.ncounters = 0;
    snprintf(g.version, sizeof g.version, "%s", version ? version : "unknown");
    snprintf(g.dir, sizeof g.dir, "%s", dir ? dir : "");
    atomic_store(&g.active, 1);
    install_crash_handlers();
    write_snapshot(g.started_ms);
    VT3_CINFO("diag", "session started (%s), metrics in %s", g.version, g.dir[0] ? g.dir : "memory only");
}

void vt3_diag_stop(void)
{
    if (!atomic_load(&g.active)) return;
    write_snapshot(now_ms());
    VT3_CINFO("diag", "%lld frames, p50 %u ms, p95 %u ms, %d log entries dropped",
              atomic_load(&g.frames), hist_pct(50), hist_pct(95), vt3_log_dropped());
    vt3_log_flush();
    if (g.crash_fd >= 0) {
        static const char line[] = "--- session end ---\n";
        write_all(g.crash_fd, line, sizeof line - 1);
        close(g.crash_fd);
        g.crash_fd = -1;
    }
    atomic_store(&g.active, 0);
}

int vt3_diag_active(void) { return atomic_load(&g.active); }

void vt3_diag_tick(void)
{
    if (!atomic_load(&g.active)) return;
    long long now = now_ms();
    if (g.last_tick_ms && now - g.last_tick_ms < 1000) return;
    vt3_audio *a = atomic_load(&g.audio);
    if (a) vt3_diag_audio(a);
    write_snapshot(now);
}
