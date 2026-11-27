/*
 * Host tests for the on-screen controls' logic and saved configuration.
 *
 *   cc -O1 -o /tmp/touchtest tools/touchtest/TouchHostTest.c \
 *      runtime/src/ui/touch_core.c runtime/src/ui/touch_config.c -Iruntime/include -lm
 *   /tmp/touchtest
 *
 * Everything the renderer decides without a screen is exercised here: the Default profile against
 * the 0.3.0 transform, the analog/d-pad state machine, menu repeat timing, gesture recognition,
 * touch ownership, the coordinate mapping, the button table and the config file's loading, saving,
 * clamping, forward compatibility and failure behaviour.
 */
#include "vt3/vt3_touch_config.h"
#include "vt3/vt3_touch_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, failures;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* Float comparison with a tolerance far below anything the controls can express. */
static void checkf(int cond, const char *what, float got, float want)
{
    (void)cond;
    checks++;
    float d = got - want;
    if (d < 0.0f) d = -d;
    if (d > 0.000001f) {
        failures++;
        printf("FAIL: %s (got %.6f, want %.6f)\n", what, got, want);
    }
}

static vt3_stick_cfg default_profile(void)
{
    vt3_stick_cfg p;
    memset(&p, 0, sizeof p);
    p.deadzone = 0.14f;
    p.sensitivity = 1.0f;
    p.range = 1.0f;
    p.sens_x = p.sens_y = 1.0f;
    return p;
}

/* 1. The Default profile reproduces the shipped transform exactly, over the whole stick. */
static void test_default_matches_original(void)
{
    vt3_stick_cfg p = default_profile();
    const float r = 168.0f;
    int exact = 1;
    for (int ix = -20; ix <= 20; ix++) {
        for (int iy = -20; iy <= 20; iy++) {
            float dx = r * 1.5f * (float)ix / 20.0f;
            float dy = r * 1.5f * (float)iy / 20.0f;
            float ox, oy, nx, ny;
            vt3_stick_apply_original(dx, dy, r, &ox, &oy);
            vt3_stick_apply(&p, dx, dy, r, &nx, &ny);
            if (ox != nx || oy != ny) exact = 0;
        }
    }
    check(exact, "Default profile is bit-identical to the 0.3.0 stick transform");

    /* And the dead zone still reads as centred. */
    float x, y;
    vt3_stick_apply(&p, 0.10f * r, 0.0f, r, &x, &y);
    checkf(x == 0.0f, "Default: inside the 14% dead zone reads zero", x, 0.0f);
    vt3_stick_apply(&p, 0.0f, 1.5f * r, r, &x, &y);
    checkf(y, "Default: full travel down clamps to 1", y, 1.0f);
    vt3_stick_apply(&p, -1.5f * r, 0.0f, r, &x, &y);
    checkf(x, "Default: full travel left clamps to -1", x, -1.0f);
}

/* 2, 3, 4. Analog <-> d-pad switching, dwell and pause/resume. */
static void test_mode_switching(void)
{
    vt3_mode_state s;
    memset(&s, 0, sizeof s);
    int changed = 0;

    check(vt3_mode_update(&s, 0, 0, 120, &changed) == VT3_TOUCH_ANALOG && !changed,
          "starts analog");
    check(vt3_mode_update(&s, 1, 1000, 120, &changed) == VT3_TOUCH_ANALOG && !changed,
          "a menu opening does not switch on the first frame");
    check(vt3_mode_update(&s, 1, 1119, 120, &changed) == VT3_TOUCH_ANALOG && !changed,
          "still analog just before the dwell expires");
    check(vt3_mode_update(&s, 1, 1120, 120, &changed) == VT3_TOUCH_DPAD && changed,
          "switches to d-pad once the menu has held for the dwell");

    /* A menu that flashes for a frame must not switch, and must not leave a pending switch either. */
    check(vt3_mode_update(&s, 0, 1300, 120, &changed) == VT3_TOUCH_DPAD && !changed,
          "returning to gameplay starts a fresh dwell");
    check(vt3_mode_update(&s, 1, 1350, 120, &changed) == VT3_TOUCH_DPAD && !changed,
          "flicker does not switch");
    check(vt3_mode_update(&s, 0, 1360, 120, &changed) == VT3_TOUCH_DPAD && !changed,
          "and it goes back to the current mode without switching");

    /* Pause and resume: d-pad in the pause menu, analog again after it closes. */
    check(vt3_mode_update(&s, 1, 2000, 120, &changed) == VT3_TOUCH_DPAD && !changed,
          "pause menu reuses the d-pad mode");
    check(vt3_mode_update(&s, 0, 2200, 120, &changed) == VT3_TOUCH_DPAD && !changed,
          "resuming starts the analog dwell");
    check(vt3_mode_update(&s, 0, 2320, 120, &changed) == VT3_TOUCH_ANALOG && changed,
          "resuming restores analog after the dwell");
    check(s.mode == VT3_TOUCH_ANALOG, "mode state records analog");
}

/* 5. Menu navigation repeat timing. */
static void test_repeat_timing(void)
{
    vt3_repeat_timing t = { 400, 120 };
    vt3_repeat_state s;
    memset(&s, 0, sizeof s);
    s.dir = VT3_DPAD_NONE;

    check(vt3_repeat_update(&s, VT3_DPAD_DOWN, 0, &t) == 1, "first press navigates immediately");
    check(vt3_repeat_update(&s, VT3_DPAD_DOWN, 200, &t) == 0, "no repeat before the delay");
    check(vt3_repeat_update(&s, VT3_DPAD_DOWN, 399, &t) == 0, "no repeat just before the delay");
    check(vt3_repeat_update(&s, VT3_DPAD_DOWN, 400, &t) == 1, "repeat at the delay");
    check(vt3_repeat_update(&s, VT3_DPAD_DOWN, 450, &t) == 0, "no repeat between intervals");
    check(vt3_repeat_update(&s, VT3_DPAD_DOWN, 520, &t) == 1, "repeat at the interval");
    check(vt3_repeat_update(&s, VT3_DPAD_DOWN, 521, &t) == 0, "one pulse per interval, not per frame");
    check(vt3_repeat_update(&s, VT3_DPAD_NONE, 600, &t) == 0, "releasing stops the repeat");
    check(vt3_repeat_update(&s, VT3_DPAD_LEFT, 700, &t) == 1, "a new direction navigates immediately");
    check(vt3_repeat_update(&s, VT3_DPAD_LEFT, 800, &t) == 0, "and repeats on its own schedule");
}

/* 6. The original button mappings, unchanged. */
static void test_button_table(void)
{
    check(vt3_touch_button_mask(VT3_TC_A) == VT3_BTN_A, "A -> A");
    check(vt3_touch_button_mask(VT3_TC_B) == VT3_BTN_B, "B -> B");
    check(vt3_touch_button_mask(VT3_TC_X) == VT3_BTN_X, "X -> X");
    check(vt3_touch_button_mask(VT3_TC_Y) == VT3_BTN_Y, "Y -> Y");
    check(vt3_touch_button_mask(VT3_TC_LB) == VT3_BTN_LB, "LB -> LB");
    check(vt3_touch_button_mask(VT3_TC_RB) == VT3_BTN_RB, "RB -> RB");
    check(vt3_touch_button_mask(VT3_TC_SELECT) == VT3_BTN_BACK, "Select -> Back");
    check(vt3_touch_button_mask(VT3_TC_START) == VT3_BTN_START, "Start -> Start");
    check(vt3_touch_button_mask(VT3_TC_STICK) == 0, "the stick sends no button of its own");
    check(vt3_touch_button_mask(VT3_TC_SWIPE) == 0, "the swipe pad sends only its mapped action");
}

/* 7, 8. Independent pointers, ownership and simultaneous movement + shot. */
static void test_ownership(void)
{
    int cand[3] = { VT3_TC_STICK, VT3_TC_A, -1 };
    float dist[3] = { 40.0f, 30.0f, 0.0f };
    check(vt3_touch_pick(cand, dist, 3, 0) == VT3_TC_A, "nearest control wins when zones overlap");
    check(vt3_touch_pick(cand, dist, 3, 1u << VT3_TC_A) == VT3_TC_STICK,
          "a control held by another finger is not stolen");
    check(vt3_touch_pick(cand, dist, 3, (1u << VT3_TC_A) | (1u << VT3_TC_STICK)) == -1,
          "no control when every candidate is already owned");

    /* Two fingers: the stick for movement, A for the shot, each owned once. */
    uint32_t owned = 0;
    int c1 = vt3_touch_pick(cand, dist, 3, owned);
    owned |= 1u << c1;
    int cand2[1] = { VT3_TC_A };
    float d2[1] = { 5.0f };
    int c2 = vt3_touch_pick(cand2, d2, 1, owned);
    check(c1 == VT3_TC_A, "first finger takes the button it landed on");
    check(c2 == -1, "the second finger cannot also take A");
    owned = 1u << VT3_TC_STICK;
    int c3 = vt3_touch_pick(cand2, d2, 1, owned);
    check(c3 == VT3_TC_A, "movement and a shot can be held at once");
}

/* 9, 10, 11. Swipe recognition, cancellation and one action per gesture. */
static void test_swipe(void)
{
    vt3_swipe_state s;
    vt3_swipe_reset(&s);
    int dir = 0;
    check(vt3_swipe_begin(&s, 7, 100, 100, 0) == 1, "a finger starts a gesture");
    check(vt3_swipe_begin(&s, 8, 120, 100, 0) == 0, "a second finger cannot start another");
    check(vt3_swipe_move(&s, 7, 100, 90, 20, &dir) == 0 && dir == VT3_DPAD_NONE,
          "a short move does not lock a direction");
    check(vt3_swipe_move(&s, 7, 100, 70, 20, &dir) == 1 && dir == VT3_DPAD_UP,
          "swiping up locks UP");
    check(vt3_swipe_locked(&s), "gesture reports locked");
    int d2 = 0;
    check(vt3_swipe_move(&s, 7, 160, 200, 20, &d2) == 0 && d2 == VT3_DPAD_UP,
          "the direction cannot change mid-gesture");
    int fdir = 0, fired = 0;
    vt3_swipe_end(&s, 7, &fdir, &fired);
    check(fdir == VT3_DPAD_UP && !fired, "the gesture reports its locked direction once");
    check(!s.active, "the gesture is finished");

    /* Incomplete swipe: cancelled, no action. */
    vt3_swipe_reset(&s);
    vt3_swipe_begin(&s, 3, 500, 500, 0);
    check(vt3_swipe_move(&s, 3, 505, 504, 40, &dir) == 0, "a nudge does not count as a swipe");
    vt3_swipe_end(&s, 3, &fdir, &fired);
    check(fdir == VT3_DPAD_NONE && !fired, "an incomplete swipe is cancelled");

    /* One action per gesture: after it fires, the same gesture cannot fire again. */
    vt3_swipe_reset(&s);
    vt3_swipe_begin(&s, 9, 0, 0, 0);
    vt3_swipe_move(&s, 9, -60, 0, 20, &dir);
    check(dir == VT3_DPAD_LEFT, "swiping left locks LEFT");
    s.fired = 1;
    check(vt3_swipe_move(&s, 9, 0, 90, 20, &dir) == 0 && dir == VT3_DPAD_LEFT,
          "a fired gesture keeps its action and cannot fire another");
    vt3_swipe_end(&s, 9, &fdir, &fired);
    check(fired == 1, "the action is reported as already sent");

    /* A release from another pointer id must not end this gesture. */
    vt3_swipe_reset(&s);
    vt3_swipe_begin(&s, 4, 0, 0, 0);
    vt3_swipe_end(&s, 5, &fdir, &fired);
    check(s.active && fdir == VT3_DPAD_NONE, "another finger's release is ignored");
}

/* Swipe direction -> control mapping, from the saved table. */
static void test_swipe_map(void)
{
    int map[4] = { VT3_TC_A, VT3_TC_B, VT3_TC_X, VT3_TC_Y };
    check(vt3_touch_swipe_ctl(map, VT3_DPAD_UP) == VT3_TC_A, "swipe up -> A");
    check(vt3_touch_swipe_ctl(map, VT3_DPAD_DOWN) == VT3_TC_B, "swipe down -> B");
    check(vt3_touch_swipe_ctl(map, VT3_DPAD_LEFT) == VT3_TC_X, "swipe left -> X");
    check(vt3_touch_swipe_ctl(map, VT3_DPAD_RIGHT) == VT3_TC_Y, "swipe right -> Y");
    check(vt3_touch_swipe_ctl(map, VT3_DPAD_NONE) == -1, "no direction, no action");
}

/* Profiles have real, distinct effects. */
static void test_profiles(void)
{
    vt3_stick_cfg d = default_profile();
    vt3_stick_cfg classic = d;
    classic.deadzone = 0.10f;
    vt3_stick_cfg dyn = d;
    dyn.progressive = 1;
    vt3_stick_cfg fast = d;
    fast.sensitivity = 2.0f;
    vt3_stick_cfg inv = d;
    inv.invert_x = 1;

    float x, y;
    vt3_stick_apply(&d, 0.12f * 168.0f, 0, 168.0f, &x, &y);
    float cx;
    vt3_stick_apply(&classic, 0.12f * 168.0f, 0, 168.0f, &cx, &y);
    check(x == 0.0f && cx > 0.0f,
          "Classic's smaller dead zone moves where Default is still centred");

    vt3_stick_apply(&d, 0.6f * 168.0f, 0, 168.0f, &x, &y);
    float px;
    vt3_stick_apply(&dyn, 0.6f * 168.0f, 0, 168.0f, &px, &y);
    check(px < x && px > 0.0f, "Dynamic is gentler than Default at part travel");

    float sx;
    vt3_stick_apply(&fast, 0.6f * 168.0f, 0, 168.0f, &sx, &y);
    check(sx > x, "200% sensitivity reaches further than Default");

    float ix;
    vt3_stick_apply(&inv, 0.6f * 168.0f, 0, 168.0f, &ix, &y);
    checkf(ix, "inverted X flips the axis", ix, -x);

    vt3_stick_cfg early = d;
    early.range = 0.5f;
    vt3_stick_apply(&early, 0.5f * 168.0f, 0, 168.0f, &x, &y);
    checkf(x, "range 50% reaches full output at half the radius", x, 1.0f);
}

/* 20. Drawable -> logical mapping through the letterbox. */
static void test_mapping(void)
{
    vt3_touch_rect dst = { 0, 100, 1920, 880 };
    float lx, ly;
    vt3_touch_map_point(960.0f, 100.0f + 440.0f, dst, 1280, 720, &lx, &ly);
    checkf(lx, "centre maps to the logical centre (x)", lx, 640.0f);
    checkf(ly, "centre maps to the logical centre (y)", ly, 360.0f);
    vt3_touch_map_point(0.0f, 100.0f, dst, 1280, 720, &lx, &ly);
    checkf(lx, "top-left maps to 0 (x)", lx, 0.0f);
    checkf(ly, "top-left maps to 0 (y)", ly, 0.0f);
    vt3_touch_map_point(1920.0f, 980.0f, dst, 1280, 720, &lx, &ly);
    checkf(lx, "bottom-right maps to the logical width", lx, 1280.0f);
    checkf(ly, "bottom-right maps to the logical height", ly, 720.0f);
    /* A tall phone letterboxed into a wide frame: the same mapping holds for 20:9. */
    vt3_touch_rect tall = { 0, 240, 1080, 1440 };
    vt3_touch_map_point(540.0f, 960.0f, tall, 1280, 720, &lx, &ly);
    checkf(lx, "20:9 centre (x)", lx, 640.0f);
    checkf(ly, "20:9 centre (y)", ly, 360.0f);
}

/* 12, 14, 15, 16, 17. The configuration file. */
static void test_config(void)
{
    char dir[] = "/tmp/vt3touchXXXXXX";
    char *d = mkdtemp(dir);
    check(d != NULL, "temp dir");
    if (!d) return;
    char path[512];
    snprintf(path, sizeof path, "%s/touch.cfg", d);

    vt3_touch_cfg a;
    vt3_touch_cfg_defaults(&a);
    check(a.layout == VT3_LAYOUT_STANDARD, "default layout is Standard");
    check(a.profile == VT3_PROFILE_DEFAULT, "default profile is Default");
    check(a.locked == 1, "layout starts locked");
    check(a.swipe_map[0] == VT3_TC_A && a.swipe_map[3] == VT3_TC_Y, "default swipe mapping A/B/X/Y");
    check(a.ctl[VT3_LAYOUT_STANDARD][VT3_TC_SWIPE].removed == 1,
          "the swipe pad is absent from the Standard layout");
    check(a.ctl[VT3_LAYOUT_STANDARD][VT3_TC_STICK].x < 0.0f,
          "Standard keeps the built-in stick position");

    /* Persistence: every field survives a save and a load. */
    a.layout = VT3_LAYOUT_CUSTOM1;
    a.profile = VT3_PROFILE_DYNAMIC;
    a.locked = 0;
    a.swipe_enabled = 1;
    a.swipe_min_dist = 0.25f;
    a.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].x = 0.81f;
    a.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].y = 0.62f;
    a.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].size = 1.3f;
    a.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].opacity = 0.7f;
    a.prof[VT3_PROFILE_CUSTOM1].deadzone = 0.22f;
    a.prof[VT3_PROFILE_CUSTOM1].sensitivity = 1.4f;
    a.prof[VT3_PROFILE_CUSTOM1].progressive = 1;
    a.prof[VT3_PROFILE_CUSTOM1].invert_y = 1;
    check(vt3_touch_cfg_save(&a, path) == 0, "save succeeds");

    vt3_touch_cfg b;
    vt3_touch_cfg_defaults(&b);
    check(vt3_touch_cfg_load(&b, path) == 0, "load succeeds");
    check(b.layout == VT3_LAYOUT_CUSTOM1 && b.profile == VT3_PROFILE_DYNAMIC,
          "layout and profile survive a restart");
    check(b.locked == 0, "the lock setting survives a restart");
    checkf(b.swipe_min_dist, "swipe distance survives", b.swipe_min_dist, 0.25f);
    checkf(b.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].x, "control x survives", b.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].x, 0.81f);
    checkf(b.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].size, "control size survives", b.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].size, 1.3f);
    checkf(b.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].opacity, "control opacity survives", b.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].opacity, 0.7f);
    checkf(b.prof[VT3_PROFILE_CUSTOM1].sensitivity, "custom profile survives", b.prof[VT3_PROFILE_CUSTOM1].sensitivity, 1.4f);
    check(b.prof[VT3_PROFILE_CUSTOM1].invert_y == 1, "axis inversion survives");

    /* Layouts are independent: editing Custom 1 left Standard alone. */
    check(b.ctl[VT3_LAYOUT_STANDARD][VT3_TC_A].x < 0.0f, "another layout keeps its own settings");

    /* 14. Restoring one control restores only that control. */
    vt3_touch_cfg c = b;
    c.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_B].removed = 1;
    c.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].removed = 1;
    c.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_B].removed = 0;   /* restore B alone */
    check(c.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].removed == 1 &&
          c.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_B].removed == 0,
          "restoring one control leaves the others as they were");

    /* Reset of one layout leaves the other layouts alone. */
    vt3_touch_cfg_preset(&c, VT3_LAYOUT_CUSTOM1);
    check(c.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].removed == 0, "reset restores a removed control");
    check(c.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].x < 0.0f, "reset returns the built-in position");
    checkf(c.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].size, "reset returns the built-in size", c.ctl[VT3_LAYOUT_CUSTOM1][VT3_TC_A].size, 1.0f);

    /* 16. Forward compatibility and clamping. */
    char bad[512];
    snprintf(bad, sizeof bad, "%s/future.cfg", d);
    FILE *f = fopen(bad, "wb");
    fprintf(f, "version=99\nlayout=compact\n");
    fclose(f);
    vt3_touch_cfg keep;
    vt3_touch_cfg_defaults(&keep);
    keep.layout = VT3_LAYOUT_CUSTOM2;
    int rc = vt3_touch_cfg_load(&keep, bad);
    check(rc == -2, "a file from a newer version is refused");
    check(keep.layout == VT3_LAYOUT_CUSTOM2, "and the caller's settings are untouched");

    snprintf(bad, sizeof bad, "%s/odd.cfg", d);
    f = fopen(bad, "wb");
    fprintf(f, "version=1\nlayout=compact\nunknown.key=7\np.custom2.deadzone=5\n"
               "l.compact.a.opacity=0\nl.compact.a.size=99\n");
    fclose(f);
    vt3_touch_cfg o;
    vt3_touch_cfg_defaults(&o);
    check(vt3_touch_cfg_load(&o, bad) == 0, "unknown keys do not fail the file");
    check(o.layout == VT3_LAYOUT_COMPACT, "the rest of an odd file still applies");
    checkf(o.prof[VT3_PROFILE_CUSTOM2].deadzone, "an out-of-range dead zone is clamped", o.prof[VT3_PROFILE_CUSTOM2].deadzone, 0.6f);
    checkf(o.ctl[VT3_LAYOUT_COMPACT][VT3_TC_A].opacity, "an invisible opacity is clamped", o.ctl[VT3_LAYOUT_COMPACT][VT3_TC_A].opacity, 0.15f);
    checkf(o.ctl[VT3_LAYOUT_COMPACT][VT3_TC_A].size, "a huge size is clamped", o.ctl[VT3_LAYOUT_COMPACT][VT3_TC_A].size, 2.5f);
    check(o.prof[VT3_PROFILE_CUSTOM1].deadzone == 0.14f, "keys that are absent keep their defaults");

    /* 17. A failed save must not destroy the previous good configuration. */
    check(vt3_touch_cfg_save(&b, path) == 0, "re-save the good file");
    chmod(d, 0500);
    vt3_touch_cfg broken = b;
    broken.layout = VT3_LAYOUT_SWIPE;
    int failed = vt3_touch_cfg_save(&broken, path);
    chmod(d, 0700);
    check(failed == -1, "a save that cannot be written reports failure");
    vt3_touch_cfg after;
    vt3_touch_cfg_defaults(&after);
    check(vt3_touch_cfg_load(&after, path) == 0 && after.layout == VT3_LAYOUT_CUSTOM1,
          "the previous valid configuration is still there after a failed save");

    /* A missing file is reported as such, not as corruption. */
    char missing[512];
    snprintf(missing, sizeof missing, "%s/none.cfg", d);
    check(vt3_touch_cfg_load(&after, missing) == -1, "a missing file is reported as missing");

    unlink(path);
    unlink(bad);
    rmdir(d);
}

int main(void)
{
    test_default_matches_original();
    test_mode_switching();
    test_repeat_timing();
    test_button_table();
    test_ownership();
    test_swipe();
    test_swipe_map();
    test_profiles();
    test_mapping();
    test_config();
    printf("%d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
