/*
 * The port's in-game UI, drawn with the renderer's 2D path:
 *
 *   the quick menu   a compact centred panel in the VT3 palette -- translucent silver, an
 *                    orange-red selected row, dark text, arrows and controller hints. Opening it
 *                    blocks this thread, so the game does not advance while it is up: a pause, not
 *                    an overlay that hides a running match.
 *
 *   the overlay      frames per second (measured from presented frames), the CPU temperature and
 *                    the battery temperature, in a translucent panel with an orange accent. The
 *                    temperatures come from markers written by ThermalWatch.java: battery from
 *                    Android's battery broadcast, CPU from a readable thermal zone. When a source
 *                    is not readable the panel says "Unavailable" -- it never substitutes one
 *                    sensor's value for another.
 *
 * Both use the same 8x8 font as the touch controls and no texture, so they cost a handful of
 * quads and cannot fail to load an asset at runtime.
 */
#include "vt3/vt3_menu.h"

#include "vt3/vt3_diag.h"
#include "vt3/vt3_input.h"
#include "vt3/vt3_log.h"
#include "vt3/vt3_platform.h"
#include "vt3/vt3_renderer.h"
#include "vt3/vt3_touch.h"

/* Provided by the Android platform layer: shows the port's main menu and parks this thread. */
void vt3_platform_request_main_menu(void);

#include "font8x8.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_VERTS 4096
#define ROW_H 40.0f
#define TEXT 2.0f

/* The VT3 palette, as the renderer wants it: 0xAARRGGBB. */
#define C_INK        0xFF2B2723u
#define C_INK_SOFT   0xFF57504Au
#define C_PANEL      0xE9EDF2F7u
#define C_PANEL_EDGE 0xFFB6C1CCu
#define C_SILVER     0xFFD2DAE2u
#define C_ORANGE     0xFFE2571Fu
#define C_ORANGE_HI  0xFFF07B3Cu
#define C_ORANGE_LO  0xFFB33A0Eu
#define C_GOLD       0xFFD9A03Cu
#define C_WHITE      0xFFFFFFFFu
#define C_SHADOW     0x55181A1Cu
#define C_DIM        0xB8181A1Cu

static struct {
    int inited;
    int quick;              /* VT3_QUICK_MENU: the menu may open */
    int overlay;            /* VT3_PERF_OVERLAY */
    int overlay_full;       /* VT3_PERF_OVERLAY_FULL */
    int overlay_pos;        /* overlay_pos marker: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right */
    int bat_src_is_kernel;  /* the battery reading came from sysfs this sample */
    int open;
    int guide;              /* the controls guide page is showing */
    int sel;
    int wait_screen;        /* a Java settings/editor screen is on top: hold the pause */
    char dir[512];          /* VT3_SETTINGS_DIR, where the temperature markers live */
    char cpu[24], bat[24], src[64], therm[24];
    uint64_t sample_at;
    uint64_t window_at;
    uint32_t frames;
    float fps;
    float frame_ms;
    uint32_t prev_buttons;
} g;

static vt3_tlvertex g_v[MAX_VERTS];
static int g_vn;

/* ---- small 2D helpers ---------------------------------------------------------------- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void push_vtx(float x, float y, uint32_t c)
{
    if (g_vn >= MAX_VERTS) return;
    vt3_tlvertex *v = &g_v[g_vn++];
    v->x = x;
    v->y = y;
    v->z = 0.0f;
    v->rhw = 1.0f;
    v->color = c;
    v->u = 0.0f;
    v->v = 0.0f;
}

static void quad(float x0, float y0, float x1, float y1, uint32_t c)
{
    push_vtx(x0, y0, c); push_vtx(x1, y0, c); push_vtx(x1, y1, c);
    push_vtx(x0, y0, c); push_vtx(x1, y1, c); push_vtx(x0, y1, c);
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c)
{
    push_vtx(x0, y0, c); push_vtx(x1, y1, c); push_vtx(x2, y2, c);
}

/* A plate with a hairline edge: the panel every VT3 menu sits on. */
static void plate(float x, float y, float w, float h, uint32_t fill, uint32_t edge)
{
    quad(x, y, x + w, y + h, edge);
    quad(x + 1, y + 1, x + w - 1, y + h - 1, fill);
}

static void glyph(char ch, float x, float y, float px, uint32_t c)
{
    unsigned u = (unsigned char)ch;
    if (u < VT3_FONT_FIRST || u > VT3_FONT_LAST) return;
    const uint8_t *gl = vt3_font8x8[u - VT3_FONT_FIRST];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = gl[row];
        int col = 0;
        while (col < 8) {
            if (bits & (1u << col)) {
                int start = col;
                while (col < 8 && (bits & (1u << col))) col++;
                quad(x + (float)start * px, y + (float)row * px,
                     x + (float)col * px, y + (float)(row + 1) * px, c);
            } else {
                col++;
            }
        }
    }
}

static void text(const char *s, float x, float y, float px, uint32_t c)
{
    for (; *s; s++, x += 8.0f * px) glyph(*s, x, y, px, c);
}

static float text_w(const char *s, float px)
{
    return (float)strlen(s) * 8.0f * px;
}

static void text_center(const char *s, float cx, float y, float px, uint32_t c)
{
    text(s, cx - text_w(s, px) * 0.5f, y, px, c);
}

/* ---- markers ------------------------------------------------------------------------- */

static void marker(const char *name, char *out, size_t cap, const char *def)
{
    char path[768];
    snprintf(path, sizeof path, "%s/%s", g.dir, name);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(out, cap, "%s", def);
        return;
    }
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) out[--n] = 0;
    if (!out[0]) snprintf(out, cap, "%s", def);
}

static void write_flag(const char *name, int on)
{
    char path[768];
    snprintf(path, sizeof path, "%s/%s", g.dir, name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(on ? "1" : "0", f);
    fclose(f);
}

static int marker_int_or(const char *name, int def)
{
    char path[768];
    snprintf(path, sizeof path, "%s/%s", g.dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return def;
    int v = def;
    if (fscanf(f, "%d", &v) != 1) v = def;
    fclose(f);
    return v;
}

/* Real readings, straight from the kernel, so the overlay works during a match with no Java
 * activity alive: the sysfs thermal zones for the CPU and the battery's own sensor. Android does
 * not guarantee every zone is readable, so a failure is reported as such, never guessed. */
static int read_number(const char *path, long *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long v = 0;
    int ok = fscanf(f, "%ld", &v) == 1;
    fclose(f);
    if (!ok) return 0;
    *out = v;
    return 1;
}

static int read_text(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return n > 0;
}

/* Some sensors report millidegrees, some decidegrees; both are normalised to whole degrees. */
static int to_degrees(long raw)
{
    if (raw > 1000 || raw < -1000) return (int)(raw / 1000);
    return (int)(raw / 10);
}

static int cpu_temperature(int *deg, char *src, size_t cap)
{
    const char *hints[] = { "cpu", "soc", "tsens", "ap", "cluster", "big", "little", "skin" };
    char zone[128], path[192], type[96];
    for (int i = 0; i < 64; i++) {
        snprintf(zone, sizeof zone, "/sys/class/thermal/thermal_zone%d", i);
        snprintf(path, sizeof path, "%s/temp", zone);
        long raw;
        if (!read_number(path, &raw)) continue;
        snprintf(path, sizeof path, "%s/type", zone);
        if (!read_text(path, type, sizeof type)) type[0] = 0;
        for (int h = 0; h < (int)(sizeof hints / sizeof hints[0]); h++) {
            if (strstr(type, hints[h])) {
                int c = to_degrees(raw);
                if (c > 0 && c < 130) {
                    *deg = c;
                    snprintf(src, cap, "%s (%.40s)", zone, type);
                    return 1;
                }
                break;
            }
        }
    }
    return 0;
}

static int battery_temperature(int *deg, char *src, size_t cap)
{
    static const char *paths[] = {
        "/sys/class/power_supply/battery/temp",
        "/sys/class/power_supply/bms/temp",
        "/sys/class/power_supply/Battery/temp"
    };
    for (int i = 0; i < (int)(sizeof paths / sizeof paths[0]); i++) {
        long raw;
        if (!read_number(paths[i], &raw)) continue;
        int c = to_degrees(raw);
        if (c > -40 && c < 130) {
            *deg = c;
            snprintf(src, cap, "%.60s", paths[i]);
            return 1;
        }
    }
    return 0;
}

static void sample(void)
{
    uint64_t now = now_ns();
    if (now < g.sample_at) return;
    g.sample_at = now + 1000000000ull;
    /* The kernel first: it is the live source during play. The markers are a fallback for a
     * device whose thermal zones are not readable and whose app can supply them. */
    int deg = 0;
    char src[96];
    if (cpu_temperature(&deg, src, sizeof src)) {
        snprintf(g.cpu, sizeof g.cpu, "%d", deg);
        snprintf(g.src, sizeof g.src, "%s", src);
    } else {
        marker("cpu_temp", g.cpu, sizeof g.cpu, "Unavailable");
        marker("cpu_temp_source", g.src, sizeof g.src, "");
    }
    if (battery_temperature(&deg, src, sizeof src)) {
        snprintf(g.bat, sizeof g.bat, "%d", deg);
        g.bat_src_is_kernel = 1;
    } else {
        marker("battery_temp", g.bat, sizeof g.bat, "Unavailable");
    }
    marker("thermal", g.therm, sizeof g.therm, "");
    /* The settings screen writes these while the game runs, so the overlay follows a toggle
     * without a restart. The markers win over the environment set at launch. */
    g.overlay = marker_int_or("perf_overlay", g.overlay) != 0;
    g.overlay_full = marker_int_or("perf_overlay_full", g.overlay_full) != 0;
    g.overlay_pos = marker_int_or("overlay_pos", g.overlay_pos);
}

/* ---- overlay ------------------------------------------------------------------------- */

/* The font is ASCII, so the degree sign is drawn: a small hollow square, superscript, after the
 * number and before the C. */
static void degree_mark(float x, float y, float px, uint32_t c)
{
    quad(x, y, x + 3 * px, y + px, c);
    quad(x, y, x + px, y + 3 * px, c);
    quad(x + 2 * px, y, x + 3 * px, y + 3 * px, c);
    quad(x, y + 2 * px, x + 3 * px, y + 3 * px, c);
}

/* "47.2" then the degree ring then "C", or the honest "Unavailable". */
static void temp_text(float x, float y, float px, const char *value, uint32_t c)
{
    if (!value || !value[0] || strcmp(value, "Unavailable") == 0) {
        text("Unavailable", x, y, px, c);
        return;
    }
    text(value, x, y, px, c);
    float after = x + text_w(value, px);
    degree_mark(after + px, y - px, px * 0.75f, c);
    text("C", after + 4.0f * px, y, px, c);
}

void vt3_menu_overlay(int drawable_w, int drawable_h)
{
    (void)drawable_w;
    (void)drawable_h;
    int lw = 0, lh = 0;
    vt3_gfx_logical_size(&lw, &lh);
    if (lw <= 0 || lh <= 0) return;
    sample();
    if (!g.overlay) return;

    char f1[32];
    snprintf(f1, sizeof f1, "%.0f", (double)g.fps);

    if (!g.overlay_full) {
        /* One line: the three numbers the overlay exists for. */
        float px = 1.6f;
        char line[64];
        snprintf(line, sizeof line, "%s FPS", g.fps > 0.5f ? f1 : "--");
        float w = text_w(line, px) + 20.0f + text_w("CPU ", px) + 34.0f
                + text_w(" BAT ", px) + 34.0f;
        float h = 8.0f * px + 12.0f;
        float bx = g.overlay_pos == 1 || g.overlay_pos == 3 ? (float)lw - w - 10.0f : 10.0f;
        float by = g.overlay_pos >= 2 ? (float)lh - h - 10.0f : 10.0f;
        plate(bx, by, w, h, C_PANEL, C_ORANGE);
        quad(bx, by, bx + 4, by + h, C_ORANGE);
        float x = bx + 10, y = by + 6;
        text(line, x, y, px, C_INK);
        x += text_w(line, px) + 14.0f;
        text("CPU ", x, y, px, C_INK_SOFT);
        temp_text(x + text_w("CPU ", px), y, px, g.cpu, C_INK);
        x += text_w("CPU ", px) + text_w("88.8", px) + 22.0f;
        text("BAT ", x, y, px, C_INK_SOFT);
        temp_text(x + text_w("BAT ", px), y, px, g.bat, C_INK);
        return;
    }

    /* Expanded: frame time, both temperatures with their own labels, the thermal step, and the
     * source the CPU reading came from so a number is never shown without its origin. */
    const char *t = "Unavailable";
    if (g.therm[0]) {
        int s = atoi(g.therm);
        static const char *names[] = { "cool", "warm", "moderate", "hot", "critical", "emergency" };
        if (s >= 0 && s < 6) t = names[s];
    }
    float px = 1.6f;
    char l1[64], l4[64];
    snprintf(l1, sizeof l1, "%s FPS   %.1f ms", g.fps > 0.5f ? f1 : "--", (double)g.frame_ms);
    snprintf(l4, sizeof l4, "Thermal: %s", t);
    float w = text_w("Battery Temp: ", px) + text_w("88.8", px) + 26.0f;
    if (text_w(l1, px) + 12.0f > w) w = text_w(l1, px) + 12.0f;
    if (text_w(l4, px) + 12.0f > w) w = text_w(l4, px) + 12.0f;
    float h = 4 * (8.0f * px + 5.0f) + 16.0f + (g.src[0] ? 10.0f : 0.0f);
    float bx = g.overlay_pos == 1 || g.overlay_pos == 3 ? (float)lw - w - 10.0f : 10.0f;
    float by = g.overlay_pos >= 2 ? (float)lh - h - 10.0f : 10.0f;
    plate(bx, by, w, h, C_PANEL, C_ORANGE);
    quad(bx, by, bx + 4, by + h, C_ORANGE);
    float y = by + 6;
    text(l1, bx + 10, y, px, C_INK); y += 8.0f * px + 5.0f;
    text("CPU Temp: ", bx + 10, y, px, C_INK_SOFT);
    temp_text(bx + 10 + text_w("CPU Temp: ", px), y, px, g.cpu, C_INK); y += 8.0f * px + 5.0f;
    text("Battery Temp: ", bx + 10, y, px, C_INK_SOFT);
    temp_text(bx + 10 + text_w("Battery Temp: ", px), y, px, g.bat, C_INK); y += 8.0f * px + 5.0f;
    text(l4, bx + 10, y, px, C_INK_SOFT);
    if (g.src[0]) {
        char src[96];
        snprintf(src, sizeof src, "CPU source: %.50s", g.src);
        text(src, bx + 10, by + h - 10.0f, 1.1f, C_INK_SOFT);
    }
}

/* ---- quick menu ---------------------------------------------------------------------- */

static const char *rows[] = { "Resume", "Touchscreen controls", "On-screen controls", "Controls guide", "Settings", "Exit to main menu" };
#define ROWS ((int)(sizeof rows / sizeof rows[0]))

static int touch_on(void)
{
    const char *e = getenv("VT3_TOUCH");
    if (!e) return 1;
    return atoi(e) != 0;
}

static void toggle_touch(void)
{
    int on = !touch_on();
    setenv("VT3_TOUCH", on ? "1" : "0", 1);
    write_flag("touch", on);
    vt3_touch_reload();
    VT3_CINFO("menu", "on-screen controls %s", on ? "on" : "off");
}

static void draw_guide(int lw, int lh)
{
    static const char *lines[] = {
        "A / Cross     top spin",
        "B / Circle    slice",
        "X / Square    lob",
        "Y / Triangle  drop shot",
        "Left stick    movement (and menus)",
        "Right stick   skill shot",
        "LB / RB       advanced shot modifier",
        "Start         game pause / main menu",
        "Back/Select   this quick menu",
    };
    int n = (int)(sizeof lines / sizeof lines[0]);
    float w = 420, h = 64 + n * 20;
    if (w > lw - 40) w = lw - 40;
    float x = (lw - w) * 0.5f, y = (lh - h) * 0.5f;
    plate(x, y, w, h, C_PANEL, C_PANEL_EDGE);
    quad(x + 1, y + 1, x + w - 1, y + 4, C_ORANGE);
    text_center("CONTROLS", lw * 0.5f, y + 14, 2.0f, C_INK);
    for (int i = 0; i < n; i++)
        text(lines[i], x + 24, y + 44 + i * 20, 1.5f, C_INK_SOFT);
    text_center("A / B   back", lw * 0.5f, y + h - 22, 1.3f, C_INK_SOFT);
}

static void draw_menu(int lw, int lh)
{
    /* The game's last frame is not kept, so the pause is drawn over a dimmed court-dark plate:
     * the VT3 pause screen dims the match behind it in exactly this way. */
    quad(0, 0, (float)lw, (float)lh, C_DIM);

    float w = lw * 0.56f;
    if (w < 320) w = 320;
    if (w > 560) w = 560;
    float h = 66 + ROWS * (ROW_H + 6) + 34;
    float x = (lw - w) * 0.5f, y = (lh - h) * 0.5f;

    /* Drop shadow, then the translucent silver panel with its orange top rule. */
    quad(x + 6, y + 8, x + w + 6, y + h + 8, C_SHADOW);
    plate(x, y, w, h, C_PANEL, C_PANEL_EDGE);
    quad(x + 1, y + 1, x + w - 1, y + 5, C_ORANGE);
    quad(x + 1, y + 5, x + w - 1, y + 6, C_GOLD);

    text_center("PAUSED", lw * 0.5f, y + 18, 2.4f, C_INK);
    text_center("VIRTUA TENNIS 3", lw * 0.5f, y + 44, 1.2f, C_INK_SOFT);

    for (int i = 0; i < ROWS; i++) {
        float ry = y + 66 + i * (ROW_H + 6);
        int on = i == g.sel;
        if (on) {
            quad(x + 16, ry, x + w - 16, ry + ROW_H, C_ORANGE);
            quad(x + 16, ry, x + w - 16, ry + 3, C_ORANGE_HI);
            quad(x + 16, ry + ROW_H - 3, x + w - 16, ry + ROW_H, C_ORANGE_LO);
            /* The navigation arrows either side of the selected row. */
            tri(x + 8, ry + ROW_H * 0.5f - 7, x + 8, ry + ROW_H * 0.5f + 7,
                x + 16, ry + ROW_H * 0.5f, C_ORANGE);
            tri(x + w - 8, ry + ROW_H * 0.5f - 7, x + w - 8, ry + ROW_H * 0.5f + 7,
                x + w - 16, ry + ROW_H * 0.5f, C_ORANGE);
        } else {
            quad(x + 16, ry, x + w - 16, ry + ROW_H, C_SILVER);
        }
        char label[96];
        if (i == 1)
            snprintf(label, sizeof label, "%s: %s", rows[i], touch_on() ? "On" : "Off");
        else
            snprintf(label, sizeof label, "%s", rows[i]);
        text(label, x + 30, ry + (ROW_H - 8 * TEXT) * 0.5f, TEXT, on ? C_WHITE : C_INK);
        if (i == 1) {
            float bx = x + w - 90;
            quad(bx, ry + 12, bx + 60, ry + ROW_H - 12, on ? C_WHITE : C_INK);
            quad(bx + 2, ry + 14, bx + 58, ry + ROW_H - 14,
                 touch_on() ? (on ? C_ORANGE : C_ORANGE) : (on ? C_ORANGE_LO : C_SILVER));
        }
    }

    text_center("A  select      B  resume      UP / DOWN  move",
                lw * 0.5f, y + h - 26, 1.2f, C_INK_SOFT);
}

static void menu_present(int dw, int dh)
{
    vt3_gfx_begin_frame();
    vt3_rect none = { 0, 0, 0, 0 };
    vt3_gfx_set_scissor(0, none);
    g_vn = 0;
    int lw = 0, lh = 0;
    vt3_gfx_logical_size(&lw, &lh);
    if (g.guide) draw_guide(lw, lh);
    else draw_menu(lw, lh);
    if (g_vn) vt3_gfx_draw(VT3_PRIM_TRIANGLELIST, g_v, g_vn, 0);
    vt3_gfx_present(dw, dh);
    vt3_platform_swap();
    vt3_platform_pump_events();
}

int vt3_menu_handle(int drawable_w, int drawable_h)
{
    vt3_menu_init();
    if (!g.open) {
        /* Open on a press of Back/Select or Guide, never on a held button. */
        uint32_t b = vt3_input_pad_buttons();
        const vt3_input_state *st = vt3_input_get();
        uint32_t merged = b | (st ? st->buttons : 0);
        uint32_t fresh = merged & ~g.prev_buttons;
        g.prev_buttons = merged;
        if (g.quick && (fresh & (VT3_BTN_BACK | VT3_BTN_GUIDE))) {
            g.open = 1;
            g.sel = 0;
            g.guide = 0;
            vt3_diag_counter("quick_menu_opens", 1);
            VT3_CINFO("menu", "quick menu opened");
        }
        return 0;
    }

    while (g.open) {
        /* One of the app's own screens (settings, the touch editor, the preview) is on top. The
         * guest stays blocked in this loop while it is, which is what keeps the match paused
         * behind it; nothing is drawn, because the window belongs to the activity meanwhile. */
        if (g.wait_screen) {
            if (!vt3_android_screen_open()) {
                g.wait_screen = 0;
                const vt3_input_state *s2 = vt3_input_get();
                g.prev_buttons = vt3_input_pad_buttons() | (s2 ? s2->buttons : 0);
                VT3_CINFO("menu", "app screen closed; still paused in the quick menu");
            } else {
                struct timespec ts = { 0, 16000000 };
                nanosleep(&ts, NULL);
            }
            continue;
        }
        menu_present(drawable_w, drawable_h);

        const vt3_input_state *st = vt3_input_get();
        uint32_t merged = vt3_input_pad_buttons() | (st ? st->buttons : 0);
        uint32_t fresh = merged & ~g.prev_buttons;
        g.prev_buttons = merged;

        if (fresh & VT3_BTN_B) {
            g.open = 0;
            g.guide = 0;
            break;
        }
        if (fresh & VT3_BTN_BACK) {
            if (g.guide) g.guide = 0;
            else g.open = 0;
            break;
        }
        if (fresh & (VT3_BTN_UP | VT3_BTN_DOWN)) {
            int dir = (fresh & VT3_BTN_UP) ? -1 : 1;
            if (g.guide) {
                /* one page: any direction returns to the rows */
                g.guide = 0;
            } else {
                g.sel = (g.sel + dir + ROWS) % ROWS;
            }
        }
        if (fresh & (VT3_BTN_A | VT3_BTN_START)) {
            if (g.guide) {
                g.guide = 0;
            } else {
                switch (g.sel) {
                    case 0:
                        g.open = 0;
                        break;
                    case 1:
                        vt3_touch_release_all();
                        VT3_CINFO("menu", "opening the touchscreen controls (match stays paused)");
                        g.wait_screen = 1;
                        vt3_android_open_touch_settings();
                        break;
                    case 2:
                        toggle_touch();
                        break;
                    case 3:
                        g.guide = 1;
                        break;
                    case 4:
                        vt3_touch_release_all();
                        VT3_CINFO("menu", "opening the settings screen (match stays paused)");
                        g.wait_screen = 1;
                        vt3_android_open_settings();
                        break;
                    case 5:
                        vt3_touch_release_all();
                        VT3_CINFO("menu", "exit to main menu");
                        vt3_platform_request_main_menu();   /* does not return */
                        break;
                    default:
                        break;
                }
            }
        }

        struct timespec ts = { 0, 16000000 };
        nanosleep(&ts, NULL);
    }
    vt3_touch_release_all();
    vt3_diag_counter("quick_menu_closes", 1);
    VT3_CINFO("menu", "quick menu closed");
    return 1;
}

int vt3_menu_open(void)
{
    return g.open;
}

void vt3_menu_set_open(int open)
{
    g.open = open ? 1 : 0;
    if (g.open) {
        g.sel = 0;
        g.guide = 0;
    }
}

/* ---- setup and frame accounting ------------------------------------------------------ */

/* Fresh state for the next match in this process: nothing carried over from the last one. */
void vt3_menu_reset(void)
{
    memset(&g, 0, sizeof g);
}

void vt3_menu_init(void)
{
    if (!g.inited) {
        g.inited = 1;
        const char *dir = getenv("VT3_SETTINGS_DIR");
        snprintf(g.dir, sizeof g.dir, "%s", dir ? dir : ".");
        const char *q = getenv("VT3_QUICK_MENU");
        g.quick = !q || atoi(q) != 0;
        snprintf(g.cpu, sizeof g.cpu, "Unavailable");
        snprintf(g.bat, sizeof g.bat, "Unavailable");
        VT3_CINFO("menu", "quick menu %s, overlay %s, markers in %s",
                  g.quick ? "on" : "off",
                  getenv("VT3_PERF_OVERLAY") ? "on" : "off", g.dir);
    }
    /* Re-read every frame: the overlay is a marker the settings screen writes while the game runs. */
    const char *o = getenv("VT3_PERF_OVERLAY");
    g.overlay = o && atoi(o) != 0;
    const char *f = getenv("VT3_PERF_OVERLAY_FULL");
    g.overlay_full = f && atoi(f) != 0;
}

void vt3_menu_tick(void)
{
    uint64_t now = now_ns();
    g.frames++;
    if (!g.window_at) g.window_at = now;
    uint64_t span = now - g.window_at;
    if (span >= 500000000ull) {
        g.fps = (float)((double)g.frames * 1e9 / (double)span);
        g.frame_ms = g.fps > 0.5f ? 1000.0f / g.fps : 0.0f;
        g.frames = 0;
        g.window_at = now;
    }
}
