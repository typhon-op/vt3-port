/*
 * Host test for the diagnostic logger and the native metrics snapshot. Not part of the game build;
 * compile it against the runtime core without any platform backend:
 *
 *   cc -O2 -std=gnu11 -Iruntime/include runtime/tests/diag_log_test.c \
 *      runtime/src/core/log.c runtime/src/core/diag.c -lpthread -o /tmp/vt3diagtest
 *   /tmp/vt3diagtest
 *
 * It checks what a host can check: the ring is bounded, every entry is either delivered or counted
 * as dropped, DEBUG is gated by detailed mode, FATAL is never lost, flush means done, the metrics
 * snapshot is complete and the crash file is opened. It does not test Android delivery (the JNI
 * sink) or the game.
 */
#include "vt3/vt3_diag.h"
#include "vt3/vt3_log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int delivered;
static int fatals;
static char last[1024];

static void sink(vt3_log_level level, const char *component, int code, const char *message, void *user)
{
    (void)user;
    (void)component;
    (void)code;
    delivered++;
    if (level == VT3_LOG_FATAL) fatals++;
    snprintf(last, sizeof last, "%s", message ? message : "");
}

static void host_test_log(void)
{
    vt3_log_set_sink(sink, NULL);
    vt3_log_set_detail(0);

    /* A flood larger than the ring: nothing may block, and nothing may vanish unseen. */
    const int n = 20000;
    for (int i = 0; i < n; i++) VT3_INFO("flood %d", i);
    vt3_log_flush();
    int dropped = vt3_log_dropped();
    printf("log: delivered %d, dropped %d (of %d)\n", delivered, dropped, n);
    assert(delivered > 0);
    assert(delivered + dropped == n);

    /* Ordering: the writer delivered the newest entry that survived the flood. */
    assert(strncmp(last, "flood ", 6) == 0);

    /* DEBUG only in detailed mode. */
    int before = delivered;
    VT3_DEBUG("hidden");
    vt3_log_flush();
    assert(delivered == before);
    vt3_log_set_detail(1);
    VT3_DEBUG("shown");
    vt3_log_flush();
    assert(delivered == before + 1);
    assert(strcmp(last, "shown") == 0);

    /* A fatal entry is delivered even when it has to wait for room. */
    int f0 = fatals;
    VT3_CFATAL("crash", 7, "fatal test entry");
    vt3_log_flush();
    assert(fatals == f0 + 1);
    assert(strcmp(last, "fatal test entry") == 0);
    vt3_log_set_detail(0);
}

static void host_test_metrics(void)
{
    char dir[] = "/tmp/vt3diagXXXXXX";
    assert(mkdtemp(dir) != NULL);
    vt3_diag_start(dir, "test-1.0");
    assert(vt3_diag_active());

    /* A second of frames with a couple of slow ones. */
    for (int i = 0; i < 60; i++) vt3_diag_frame(16000000ull);
    vt3_diag_frame(90000000ull);
    vt3_diag_render_size(1280, 720);
    vt3_diag_window_size(2400, 1080);
    vt3_diag_gpu(0x13B5, "Test GPU", (1u << 22) | (3u << 12), "VK_KHR_surface,VK_KHR_swapchain");
    vt3_diag_vulkan_error();
    vt3_diag_event("pause");
    vt3_diag_event("resume");
    vt3_diag_counter("audio_reopen_attempts", 2);
    vt3_diag_tick();
    vt3_diag_stop();

    char path[512];
    snprintf(path, sizeof path, "%s/diag_metrics.txt", dir);
    FILE *f = fopen(path, "r");
    assert(f != NULL);
    char text[8192];
    size_t got = fread(text, 1, sizeof text - 1, f);
    text[got] = 0;
    fclose(f);

    assert(strstr(text, "version=test-1.0") != NULL);
    assert(strstr(text, "frames=61") != NULL);
    assert(strstr(text, "frame_p50_ms=16") != NULL);
    assert(strstr(text, "frame_max_ms=90.0") != NULL);
    assert(strstr(text, "render_w=1280") != NULL);
    assert(strstr(text, "window_h=1080") != NULL);
    assert(strstr(text, "gpu_name=Test GPU") != NULL);
    assert(strstr(text, "gpu_api=1.3") != NULL);
    assert(strstr(text, "gpu_vendor=ARM") != NULL);
    assert(strstr(text, "vulkan_errors=1") != NULL);
    assert(strstr(text, "count_pause=1") != NULL);
    assert(strstr(text, "count_resume=1") != NULL);
    assert(strstr(text, "count_audio_reopen_attempts=2") != NULL);

    /* The crash file exists from start-up and records the session boundaries. */
    char crash[512];
    snprintf(crash, sizeof crash, "%s/native_crash.txt", dir);
    f = fopen(crash, "r");
    assert(f != NULL);
    got = fread(text, 1, sizeof text - 1, f);
    text[got] = 0;
    fclose(f);
    assert(strstr(text, "session start") != NULL);
    assert(strstr(text, "session end") != NULL);

    printf("metrics: snapshot and crash file verified in %s\n", dir);
    unlink(path);
    unlink(crash);
    rmdir(dir);
}

int main(void)
{
    host_test_log();
    host_test_metrics();
    printf("diag_log_test: all checks passed\n");
    return 0;
}
