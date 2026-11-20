/*
 * Platform layer on Android: the NativeActivity window, its lifecycle events and its input queue.
 * android_main (src/platform/android_entry.c) owns the android_app and calls the handlers here
 * from the glue's looper, so everything runs on the game thread. The renderer presents to the
 * window through vt3_platform_native_window; there is no GL context.
 *
 * Lifecycle state lives in two flags, not in the window pointer alone:
 *   g_paused  the activity asked for no work (APP_CMD_PAUSE, cleared on APP_CMD_RESUME). Audio
 *             is paused with it, before the surface goes away.
 *   g_window  the current ANativeWindow, NULL between APP_CMD_TERM_WINDOW and the next
 *             APP_CMD_INIT_WINDOW (sleep/wake, a configuration recreate). Audio is paused
 *             while it is NULL, because nothing is being presented either.
 * The DirectSound mixer freezes its play clocks for a paused output, so resuming continues the
 * stream rather than jumping into content the writer never produced while the app was away.
 */
#include "vt3/vt3_audio.h"
#include "vt3/vt3_log.h"
#include "vt3/vt3_platform.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <stdatomic.h>
#include <string.h>
#include <time.h>

/* Defined in src/input/input_android.c */
int vt3_input_android_event(const AInputEvent *e);
/* Defined in src/ui/touch_controls.c */
void vt3_touch_release_all(void);

#define EVENT_QUEUE 32
/* How long vt3_platform_init waits for the first surface before it gives up and reports. A cold
 * start delivers APP_CMD_INIT_WINDOW in well under a second; a window that never arrives means
 * there is nothing to draw on, so waiting for ever would leave the game in a black screen with a
 * live process (and no diagnostics) instead of a clean exit. */
#define INIT_WINDOW_TIMEOUT_MS 10000

static struct android_app *g_app;
static ANativeWindow *g_window;
static int g_quit;
static int g_paused;
static vt3_event g_queue[EVENT_QUEUE];
static int g_qhead, g_qcount;

void vt3_config_defaults(vt3_config *c)
{
    c->title = "VT3";
    c->window_w = 1280;
    c->window_h = 960;
    c->logical_w = 640;
    c->logical_h = 480;
    c->aspect_w = 4;
    c->aspect_h = 3;
    c->integer_scale = 0;
    c->vsync = 1;
    c->audio = 1;
    c->frames = 0;
    c->game_root = NULL;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void push_event(vt3_event_kind kind, int w, int h)
{
    if (g_qcount == EVENT_QUEUE) return;     /* a full queue drops the newest event */
    vt3_event *e = &g_queue[(g_qhead + g_qcount) % EVENT_QUEUE];
    e->kind = kind;
    e->w = w;
    e->h = h;
    g_qcount++;
}

/* Sound follows the lifecycle: paused while the activity is paused or has no window, resumed
 * once both are back. Called from the glue's command callback, on the game thread. */
static void apply_audio_state(void)
{
    vt3_audio_pause(g_paused || !g_window);
}

/* The glue's command callback. The game does not start until the window exists. */
void vt3_platform_android_cmd(struct android_app *app, int32_t cmd)
{
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        g_window = app->window;
        if (g_window) {
            int w = ANativeWindow_getWidth(g_window), h = ANativeWindow_getHeight(g_window);
            VT3_INFO("android: window %dx%d", w, h);
            /* The renderer rebuilds its swapchain when it sees a new window, and this tells the
             * game-side frame code the drawable changed as well. */
            push_event(VT3_EV_RESIZE, w, h);
        }
        apply_audio_state();
        break;
    case APP_CMD_TERM_WINDOW:
        g_window = NULL;
        apply_audio_state();
        vt3_touch_release_all();          /* no surface: drop anything the controls held */
        push_event(VT3_EV_BLUR, 0, 0);
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        if (g_window) {
            push_event(VT3_EV_RESIZE, ANativeWindow_getWidth(g_window), ANativeWindow_getHeight(g_window));
        }
        break;
    case APP_CMD_GAINED_FOCUS:
        push_event(VT3_EV_FOCUS, 0, 0);
        break;
    case APP_CMD_LOST_FOCUS:
        vt3_touch_release_all();
        push_event(VT3_EV_BLUR, 0, 0);
        break;
    case APP_CMD_PAUSE:
        g_paused = 1;
        apply_audio_state();
        break;
    case APP_CMD_RESUME:
        g_paused = 0;
        apply_audio_state();              /* a no-op until the window comes back */
        break;
    case APP_CMD_DESTROY:
        g_quit = 1;
        g_paused = 1;
        vt3_audio_pause(1);               /* no burst of sound while the app is torn down */
        push_event(VT3_EV_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

/* The glue's input callback: keys and pads go to the input layer. Returns 1 when handled. */
int vt3_platform_android_input(struct android_app *app, AInputEvent *e)
{
    (void)app;
    return vt3_input_android_event(e);
}

void vt3_platform_android_attach(struct android_app *app)
{
    g_app = app;
    app->onAppCmd = vt3_platform_android_cmd;
    app->onInputEvent = vt3_platform_android_input;
}

int vt3_platform_init(const vt3_config *c)
{
    (void)c;
    if (!g_app) {
        VT3_ERROR("vt3_platform_init: no android_app (the entry point did not attach)");
        return -1;
    }
    /* Pump until the window exists, for a bounded time. A blocking pollOnce(-1) with no deadline
     * is what used to hang here when the surface never came: no window, no error, no exit. */
    uint64_t deadline = now_ms() + INIT_WINDOW_TIMEOUT_MS;
    int announced = 0;
    while (!g_window && !g_quit) {
        if (!announced) {
            VT3_INFO("android: waiting up to %d ms for the window", INIT_WINDOW_TIMEOUT_MS);
            announced = 1;
        }
        uint64_t left = now_ms() < deadline ? deadline - now_ms() : 0;
        if (left == 0) break;
        int events = 0;
        struct android_poll_source *source = NULL;
        int timeout = left > 100u ? 100 : (int)left;     /* wake up to notice the deadline */
        if (ALooper_pollOnce(timeout, NULL, &events, (void **)&source) >= 0 && source)
            source->process(g_app, source);
        if (g_app->destroyRequested) g_quit = 1;
    }
    if (g_quit) {
        VT3_INFO("android: destroyed before a window arrived");
        return -1;
    }
    if (!g_window) {
        VT3_ERROR("vt3_platform_init: no window after %d ms; there is no surface to draw on",
                  INIT_WINDOW_TIMEOUT_MS);
        return -1;
    }
    return 0;
}

void vt3_platform_shutdown(void)
{
    vt3_audio_pause(1);
    g_window = NULL;
}

int vt3_platform_pump_events(void)
{
    if (!g_app) return 0;
    for (;;) {
        int events = 0;
        struct android_poll_source *source = NULL;
        int rc = ALooper_pollOnce(0, NULL, &events, (void **)&source);
        if (rc < 0) break;                     /* ALOOPER_POLL_TIMEOUT: nothing pending */
        if (source) source->process(g_app, source);
        if (g_app->destroyRequested) g_quit = 1;
    }
    /* Backgrounded (no window): block here with the sound paused, or the game loop spins a core.
     * This thread runs the glue's looper, so the window comes back through this same wait. The
     * glue's own window is tested, not g_window, which a headless shutdown also clears. */
    if (!g_app->window && !g_quit) {
        vt3_audio_pause(1);
        while (!g_app->window && !g_quit) {
            int events = 0;
            struct android_poll_source *source = NULL;
            if (ALooper_pollOnce(-1, NULL, &events, (void **)&source) >= 0 && source) source->process(g_app, source);
            if (g_app->destroyRequested) g_quit = 1;
        }
        /* The window is back, but the activity may still be paused (APP_CMD_RESUME comes with
         * it), so restore the lifecycle state instead of resuming unconditionally. */
        if (!g_quit) apply_audio_state();
    }
    vt3_audio_recover();                      /* retries a failed open/reopen with a backoff */
    return !g_quit;
}

int vt3_platform_next_event(vt3_event *ev)
{
    if (g_qcount == 0) return 0;
    *ev = g_queue[g_qhead];
    g_qhead = (g_qhead + 1) % EVENT_QUEUE;
    g_qcount--;
    return 1;
}

void vt3_platform_drawable_size(int *w, int *h)
{
    if (g_window) {
        *w = ANativeWindow_getWidth(g_window);
        *h = ANativeWindow_getHeight(g_window);
    } else {
        *w = 0;
        *h = 0;
    }
}

void vt3_platform_swap(void)
{
    /* The renderer presents its own swapchain. */
}

void *vt3_platform_gl_proc(const char *name)
{
    (void)name;
    return NULL;
}

void *vt3_platform_native_window(void)
{
    return g_window;
}
