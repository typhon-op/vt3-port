/*
 * android_main: the NativeActivity entry point of the Android build. It attaches the glue to the
 * platform layer and runs the translated game on this thread. The game's data and image come from
 * VT3_ANDROID_ROOT (device storage, pushed with adb), so nothing is bundled in the APK.
 */
#include "vt3/vt3_diag.h"
#include "vt3/vt3_guest.h"
#include "vt3/vt3_log.h"
#include "vt3/vt3_platform.h"
#include "vt3rt.h"

#include <android/native_activity.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dirent.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

/* Defined in src/platform/platform_android.c */
void vt3_platform_android_attach(struct android_app *app);
void vt3_platform_request_main_menu(void);

#include <stdatomic.h>

#define VT3_ANDROID_ROOT "/sdcard/Android/data/net.vt3port.game/files"

/* A device has no shell to set the VT3_* environment in, so read them from a small KEY=VALUE file
 * beside the game data (blank lines and # comments ignored). Every debug switch in the port is an
 * environment variable, and this is how they are reached on the Retroid. Values already in the
 * environment win, so a developer's own setting is never overwritten. */
static void apply_env_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\r' || *s == 0) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *val = eq + 1;
        size_t n = strlen(val);
        while (n && (val[n - 1] == '\n' || val[n - 1] == '\r' || val[n - 1] == ' ' || val[n - 1] == '\t'))
            val[--n] = 0;
        if (getenv(s) == NULL) setenv(s, val, 0);
        VT3_CINFO("app", "env %s=%s", s, getenv(s));
    }
    fclose(f);
}

#ifdef VT3_PGO_GEN
/* Profile-guided build, collecting: the app is force-stopped rather than exiting, so write the
 * counters every 20 s. */
int __llvm_profile_write_file(void);
void __llvm_profile_set_filename(const char *name);
static void *pgo_dump(void *arg)
{
    (void)arg;
    __llvm_profile_set_filename(VT3_ANDROID_ROOT "/vt3.profraw");
    for (;;) {
        sleep(20);
        __llvm_profile_write_file();
    }
    return NULL;
}
#endif

/* The Settings screen writes effective values as marker files (GraphicsProfile.java resolves the
 * device tier, the chosen preset, the per-option overrides and the thermal rung into them). Each
 * marker is the same lever as its VT3_* variable, so a value in vt3.env or the environment wins. */
static int marker_int(const char *name, int def)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s", VT3_ANDROID_ROOT, name);
    FILE *f = fopen(path, "r");
    if (!f) return def;
    int v = def;
    if (fscanf(f, "%d", &v) != 1) v = def;
    fclose(f);
    return v;
}

static void set_int_env(const char *name, int v)
{
    char val[16];
    snprintf(val, sizeof val, "%d", v);
    if (getenv(name) == NULL) setenv(name, val, 0);
    VT3_CINFO("settings", "%s=%s", name, val);
}

static void apply_settings(void)
{
    /* The render thread polls this directory for the thermal status and the anisotropy cap. */
    setenv("VT3_SETTINGS_DIR", VT3_ANDROID_ROOT, 1);
    VT3_CDEBUG("settings", "reading settings markers from %s", VT3_ANDROID_ROOT);
    int fps = marker_int("fps_cap", -1);
    if (fps < 0 && access(VT3_ANDROID_ROOT "/fps60", F_OK) == 0) fps = 60;  /* older shortcut */
    if (fps >= 0) set_int_env("VT3_FPS_CAP", fps);
    int h = marker_int("render_h", 0);
    if (h >= 240) set_int_env("VT3_RENDER_HEIGHT", h);
    int aniso = marker_int("aniso", 0);
    if (aniso > 0) set_int_env("VT3_ANISO", aniso);
    int pct = marker_int("render_scale_pct", 0);
    if (pct >= 40 && pct <= 100) {
        char val[16];
        snprintf(val, sizeof val, "%.2f", pct / 100.0);
        if (getenv("VT3_RENDER_SCALE") == NULL) setenv("VT3_RENDER_SCALE", val, 0);
        VT3_INFO("android: VT3_RENDER_SCALE=%s", val);
    }

    /* The rest of the settings screen, as flags. Each marker is only turned into an environment
     * variable the engine already understands, so no option exists here that the runtime does not
     * read: widescreen and the variance shadow maps are the game's own renderer switches, the
     * overlay and the quick menu are this port's, and volume/mute are the mixer's. */
    if (marker_int("no_vsm", 0)) setenv("VT3_NO_VSM", "1", 0);
    if (marker_int("no_widescreen", 0)) setenv("VT3_NO_WIDESCREEN", "1", 0);
    set_int_env("VT3_PERF_OVERLAY", marker_int("perf_overlay", 0));
    set_int_env("VT3_PERF_OVERLAY_FULL", marker_int("perf_overlay_full", 0));
    set_int_env("VT3_TOUCH", marker_int("touch", 1));
    set_int_env("VT3_TOUCH_FORCE", marker_int("touch_force", 0));
    int tscale = marker_int("touch_scale_pct", 0);
    if (tscale >= 40 && tscale <= 250) {
        char val[16];
        snprintf(val, sizeof val, "%.2f", tscale / 100.0);
        setenv("VT3_TOUCH_SCALE", val, 0);
    }
    int talpha = marker_int("touch_alpha_pct", 0);
    if (talpha >= 10 && talpha <= 100) {
        char val[16];
        snprintf(val, sizeof val, "%.2f", talpha / 100.0);
        setenv("VT3_TOUCH_ALPHA", val, 0);
    }
    if (marker_int("touch_mono", 0)) setenv("VT3_TOUCH_COLOR", "0", 0);
    int vol = marker_int("audio_volume", 100);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    set_int_env("VT3_AUDIO_VOLUME", vol);
    set_int_env("VT3_AUDIO_MUTE", marker_int("audio_mute", 0));
    if (marker_int("trace_audio", 0)) setenv("VT3_TRACE_AUDIO", "1", 0);
    if (marker_int("trace_fps", 0)) setenv("VT3_TRACE_FPS", "1", 0);
}

/* Diagnostics come up before anything else, so a failure in the rest of start-up is recorded. The
 * setup screen writes the diag_detail marker when the player turns detailed logging on; the
 * environment still wins, as with every other switch here. */
static void start_diagnostics(void)
{
    char marker[256];
    snprintf(marker, sizeof marker, "%s/diag_detail", VT3_ANDROID_ROOT);
    if (access(marker, F_OK) == 0) setenv("VT3_LOG_DETAIL", "1", 0);
    vt3_diag_start(VT3_ANDROID_ROOT, "0.2.0");
    VT3_CINFO("app", "VT3 Android 0.2.0 starting; data root %s; detailed logging %s",
              VT3_ANDROID_ROOT, getenv("VT3_LOG_DETAIL") ? "on" : "off");
}

static int dir_ok(const char *p)
{
    DIR *d = opendir(p);
    if (!d) return 0;
    closedir(d);
    return 1;
}

/* The setup screen can install the game data to a folder of the player's choosing (all files
 * access) or copy a picked folder into app storage. Either way it writes the real path of the data
 * root to <root>/game_root, and only that file is consulted here: a content:// URI never reaches
 * this layer, and a stale path falls back to app storage instead of failing the launch. */
static void apply_game_root(void)
{
    char marker[512], chosen[512] = "";
    snprintf(marker, sizeof marker, "%s/game_root", VT3_ANDROID_ROOT);
    FILE *f = fopen(marker, "r");
    if (f) {
        if (fgets(chosen, sizeof chosen, f)) {
            size_t n = strlen(chosen);
            while (n && (chosen[n - 1] == '\n' || chosen[n - 1] == '\r' || chosen[n - 1] == ' '))
                chosen[--n] = 0;
        }
        fclose(f);
    }
    if (chosen[0] == '/' && dir_ok(chosen)) {
        setenv("VT3_GAME_ROOT", chosen, 1);
        VT3_CINFO("app", "game data at %s (chosen in setup)", chosen);
        return;
    }
    if (chosen[0]) VT3_CWARN("app", "%s is not readable, using app storage", chosen);
    setenv("VT3_GAME_ROOT", VT3_ANDROID_ROOT "/data2", 1);
}

/* Counts launches in this process. The marker-derived VT3_* variables survive a match (they were
 * set with overwrite 0), so on a relaunch they are dropped and re-applied: a setting changed in the
 * menu between matches takes effect in the next one. */
static int g_launch;

/* 1 while a guest thread is running in this process. The lifecycle in one place:
 *
 *   android_main (thread A)  -> vt3_run -> guest ... Exit to main menu
 *        -> vt3_platform_request_main_menu(): renderer/audio/menu/touch/input released,
 *           MainMenu started, game activity finished, thread A ends (pthread_exit)
 *        -> this cleanup handler drops the flag, so thread A's stack is gone with the arena
 *           still mapped
 *   android_main (thread B)  -> sees g_launch > 0, waits for the flag, unmaps the guest arena and
 *           frees the recompiler tables, then vt3_run maps a fresh arena and reloads the image:
 *           a real cold start of the game inside the same process, with no second guest thread
 *           and no memory carried over from the previous match.
 */
static atomic_int g_guest_thread_alive;

static void guest_thread_done(void *arg)
{
    (void)arg;
    atomic_store(&g_guest_thread_alive, 0);
}

/* Runs on the new launch's thread, which is the only thread left, so the arena and the tables can
 * be released here safely. The wait is bounded: a teardown that never finishes must not deadlock
 * the next match, and the log says so if it happens. */
static int release_previous_match(void)
{
    for (int i = 0; i < 500 && atomic_load(&g_guest_thread_alive); i++) usleep(10 * 1000);
    if (atomic_load(&g_guest_thread_alive)) {
        /* That thread may still be running guest code on the arena and reading the import table.
         * Unmapping or freeing either would turn a stuck thread into a crash, so nothing is
         * released and this launch is refused: the caller logs and closes the activity. */
        VT3_CERROR("app", 0,
                   "refusing to start a match: the previous match's thread is still running after "
                   "5 s, so the guest arena and recompiler tables were left untouched. Close the "
                   "app (main menu -> Quit) and launch it again.");
        return -1;
    }
    vt3_runtime_forget();
    vt3_guest_shutdown();
    VT3_CINFO("app", "previous match released: guest arena unmapped, recompiler tables freed");
    return 0;
}

static void forget_marker_env(void)
{
    static const char *names[] = {
        "VT3_FPS_CAP", "VT3_RENDER_HEIGHT", "VT3_ANISO", "VT3_RENDER_SCALE",
        "VT3_NO_VSM", "VT3_NO_WIDESCREEN", "VT3_PERF_OVERLAY", "VT3_PERF_OVERLAY_FULL",
        "VT3_TOUCH", "VT3_TOUCH_FORCE", "VT3_TOUCH_SCALE", "VT3_TOUCH_ALPHA", "VT3_TOUCH_COLOR",
        "VT3_AUDIO_VOLUME", "VT3_AUDIO_MUTE", "VT3_TRACE_AUDIO", "VT3_TRACE_FPS",
        "VT3_GAME_ROOT", "VT3_SHADER_PACK"
    };
    for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++) unsetenv(names[i]);
    VT3_CINFO("app", "relaunch: re-reading the settings markers");
}

void android_main(struct android_app *app)
{
    if (g_launch++ > 0 && release_previous_match() != 0) {
        /* Nothing was freed, so this activity must not run the game: it would allocate a second
         * runtime over a match that never stopped. Close it and leave the process to the menu. */
        VT3_CINFO("app", "match launch refused; returning to the menu");
        ANativeActivity_finish(app->activity);
        return;
    }

    pthread_cleanup_push(guest_thread_done, NULL);
    atomic_store(&g_guest_thread_alive, 1);
#ifdef VT3_PGO_GEN
    pthread_t pgo;
    pthread_create(&pgo, NULL, pgo_dump, NULL);
#endif
    vt3_platform_android_attach(app);
    apply_env_file(VT3_ANDROID_ROOT "/vt3.env");
    apply_settings();
    start_diagnostics();
    /* Full screen: the game draws over the whole panel, with no status bar (the activity has no Java). */
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_FULLSCREEN, 0);
    /* Game data: <root>/data2 by default (the extracted English archive set), or the folder setup
     * recorded in <root>/game_root; <root>/image.bin is always the image. */
    apply_game_root();
    setenv("VT3_SHADER_PACK", VT3_ANDROID_ROOT "/shaders.pack", 1);
    VT3_CINFO("app", "running the game from %s", VT3_ANDROID_ROOT);
    int rc = vt3_run(&vt3_mod, VT3_ANDROID_ROOT "/image.bin");
    VT3_CINFO("app", "game returned %d", rc);
    vt3_diag_stop();
    /* The system is tearing the activity down (the task was swiped away): let the glue finish and
     * do not start the menu on top of a dying activity. The next launch still runs the release
     * handshake, because g_launch has moved on. */
    int destroying = app->destroyRequested;
    /* The guest can leave the match on its own (its ExitProcess is redirected, but a return from
     * the entry point lands here). Leaving a match never ends the process: it goes through the same
     * teardown and lands on the main menu, which never returns. rt3_platform_request_main_menu
     * ends this thread, and the cleanup handler above releases the flag for the next launch. */
    if (!destroying) {
        vt3_platform_request_main_menu();   /* releases the match, opens the menu, ends this thread */
    }
    pthread_cleanup_pop(1);   /* only reached when the activity was being destroyed */
}
