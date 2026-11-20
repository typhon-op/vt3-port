/*
 * android_main: the NativeActivity entry point of the Android build. It attaches the glue to the
 * platform layer and runs the translated game on this thread. The game's data and image come from
 * VT3_ANDROID_ROOT (device storage, pushed with adb), so nothing is bundled in the APK.
 */
#include "vt3/vt3_diag.h"
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

void android_main(struct android_app *app)
{
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
    VT3_CINFO("app", "game returned %d; shutting down", rc);
    vt3_diag_stop();
    /* Close the activity and end the process, so a relaunch never runs over stale statics. */
    ANativeActivity_finish(app->activity);
    while (!app->destroyRequested) {
        int events = 0;
        struct android_poll_source *source = NULL;
        if (ALooper_pollOnce(-1, NULL, &events, (void **)&source) >= 0 && source) source->process(app, source);
    }
    exit(rc);
}
