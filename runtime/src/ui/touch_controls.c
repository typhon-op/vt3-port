/*
 * On-screen touch controls (see vt3/vt3_touch.h).
 *
 * The overlay is drawn as translucent vector shapes through the fixed-function (XYZRHW) path, so
 * it looks the same on Vulkan and OpenGL and scales with the frame. Geometry is built in the
 * renderer's logical pixels; touch points arrive in drawable pixels and are mapped back through the
 * present letterbox (vt3_touch_map_point), so a control is hit where it is drawn even when the
 * frame is letterboxed.
 *
 * Everything a player can change lives in touch.cfg (vt3_touch_config.h): the layout's per-control
 * position, size, opacity and presence, the stick profile, the swipe pad and the lock. The Standard
 * layout with the Default profile reproduces the 0.3.0 geometry and stick response exactly; the
 * regression test in tools/touchtest holds that to the original arithmetic.
 *
 * The left stick follows the game: in a match it is an analog stick, and while a menu is open it is
 * a digital d-pad with menu repeat timing. The mode switch, the repeat timing, the gesture
 * recognition and the touch-to-control ownership all come from touch_core.c, which is host-tested.
 */
#include "vt3/vt3_log.h"
#include "vt3/vt3_input.h"
#include "vt3/vt3_menu.h"
#include "vt3/vt3_renderer.h"
#include "vt3/vt3_touch.h"
#include "vt3/vt3_touch_config.h"
#include "vt3/vt3_touch_core.h"

#include <math.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "font8x8.h"

#define TOUCH_MAX_PTR 10
#define MENU_DWELL_MS 140u
#define DPAD_PULSE_MS 55u

typedef struct {
    int   used;
    int   id;
    int   ctl;              /* VT3_TC_* the pointer is driving, -1 while unassigned */
    float lx, ly;           /* last position, logical pixels */
    float ax, ay;           /* floating stick: where this finger put the centre */
} touch_ptr;

/* One control's geometry for this frame, in logical pixels. */
typedef struct {
    float cx, cy;
    float r;                /* radius: stick, buttons, swipe pad half-extent */
    float w, h;             /* pills */
} ctl_geom;

static struct {
    int   inited;
    int   enabled;          /* VT3_TOUCH */
    int   force;            /* VT3_TOUCH_FORCE */
    int   color;
    float user_scale;
    float alpha;            /* global marker opacity, multiplied into every control */
    char  cfg_path[512];
    vt3_touch_cfg cfg;
    int   cfg_state;        /* 0 loaded, -1 missing, -2 bad: logged once */

    int      have_view;
    int      logical_w, logical_h;
    vt3_rect dst;

    float s;
    ctl_geom geo[VT3_TC_COUNT];
    int      present[VT3_TC_COUNT];
    float    opacity[VT3_TC_COUNT];

    touch_ptr ptr[TOUCH_MAX_PTR];
    float knob_x, knob_y;
    uint32_t draw_mask;
    int stick_held;

    vt3_mode_state   mode;
    vt3_repeat_state rep;
    vt3_swipe_state  swipe;
    uint32_t dpad_mask;
    uint32_t dpad_until_ms;
    int      last_dir;

    /* Draw calls per presented frame, for telling a menu from a match (see guest_in_menu). */
    unsigned draws_ring[64];
    int      draws_at;
    unsigned draws_max;
    int      guest_menu;

    uint32_t applied_mask;
    float    applied_lx, applied_ly;
    uint32_t pulse_mask;    /* d-pad bits being held for one pulse */

    vt3_tlvertex *v;
    uint32_t vn, vcap;
    int      shadow;
} g;

static void release_state(void);   /* defined with the input plumbing below */

/* ---- small helpers -------------------------------------------------------- */

#define VT3_PI 3.14159265358979323846f

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static uint32_t rgba(int r, int gr, int b, float a)
{
    a *= g.alpha;
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    return ((uint32_t)(a * 255.0f + 0.5f) << 24) | ((uint32_t)r << 16) | ((uint32_t)gr << 8) | (uint32_t)b;
}

static uint32_t accent(int ctl, float a)
{
    if (!g.color) return rgba(220, 227, 240, a);
    switch (ctl) {
    case VT3_TC_A: return rgba(112, 202, 112, a);
    case VT3_TC_B: return rgba(236, 94, 100, a);
    case VT3_TC_X: return rgba(98, 156, 242, a);
    case VT3_TC_Y: return rgba(240, 196, 72, a);
    default:       return rgba(220, 227, 240, a);
    }
}

/* ---- vertex batch --------------------------------------------------------- */

static int ensure_verts(uint32_t add)
{
    if (g.vn + add <= g.vcap) return 1;
    uint32_t cap = g.vcap ? g.vcap : 1024;
    while (cap < g.vn + add) cap *= 2;
    vt3_tlvertex *nv = realloc(g.v, (size_t)cap * sizeof *nv);
    if (!nv) return 0;
    g.v = nv;
    g.vcap = cap;
    return 1;
}

static float g_sh_x, g_sh_y;

static void push_vtx(float x, float y, uint32_t c)
{
    if (g.vn >= g.vcap) return;
    if (g.shadow) {
        uint32_t a = ((c >> 24) & 0xffu) * 62u / 100u;
        c = (a << 24);
        x += g_sh_x; y += g_sh_y;
    }
    vt3_tlvertex *t = &g.v[g.vn++];
    t->x = x; t->y = y; t->z = 0.0f; t->rhw = 1.0f;
    t->color = c; t->u = 0.0f; t->v = 0.0f;
}

static void push_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c)
{
    if (!ensure_verts(3)) return;
    push_vtx(x0, y0, c); push_vtx(x1, y1, c); push_vtx(x2, y2, c);
}

static void push_quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c)
{
    push_tri(x0, y0, x1, y1, x2, y2, c);
    push_tri(x0, y0, x2, y2, x3, y3, c);
}

static void emit_rect(float x, float y, float w, float h, uint32_t c)
{
    push_quad(x, y, x + w, y, x + w, y + h, x, y + h, c);
}

static void emit_line(float x0, float y0, float x1, float y1, float th, uint32_t c)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return;
    float nx = -dy / len * th * 0.5f, ny = dx / len * th * 0.5f;
    push_quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

static int arc_segments(float r)
{
    int seg = (int)(r * 0.9f);
    if (seg < 10) seg = 10;
    if (seg > 96) seg = 96;
    return seg;
}

static void emit_arc(float cx, float cy, float r_out, float th, float a0, float a1, uint32_t c)
{
    float r = r_out - th * 0.5f;
    if (r < 0.5f) return;
    int seg = arc_segments(r);
    if (fabsf(a1 - a0) < 3.0f) seg = seg / 4 + 1;
    if (seg < 3) seg = 3;
    float inner = r - th * 0.5f < 0.0f ? 0.0f : r - th * 0.5f;
    float outer = r + th * 0.5f;
    for (int i = 0; i < seg; i++) {
        float t0 = a0 + (a1 - a0) * (float)i / (float)seg;
        float t1 = a0 + (a1 - a0) * (float)(i + 1) / (float)seg;
        float c0 = cosf(t0), s0 = sinf(t0), c1 = cosf(t1), s1 = sinf(t1);
        push_quad(cx + inner * c0, cy + inner * s0, cx + outer * c0, cy + outer * s0,
                  cx + outer * c1, cy + outer * s1, cx + inner * c1, cy + inner * s1, c);
    }
}

static void emit_ring(float cx, float cy, float r_out, float th, uint32_t c)
{
    emit_arc(cx, cy, r_out, th, 0.0f, 2.0f * VT3_PI, c);
}

static void emit_disc(float cx, float cy, float r, uint32_t c)
{
    if (r < 0.5f) return;
    int seg = arc_segments(r) + 8;
    for (int i = 0; i < seg; i++) {
        float t0 = 2.0f * VT3_PI * (float)i / (float)seg;
        float t1 = 2.0f * VT3_PI * (float)(i + 1) / (float)seg;
        push_tri(cx, cy, cx + r * cosf(t0), cy + r * sinf(t0), cx + r * cosf(t1), cy + r * sinf(t1), c);
    }
}

static void emit_round_rect(float x, float y, float w, float h, float rad, float th, uint32_t c)
{
    if (rad > w * 0.5f) rad = w * 0.5f;
    if (rad > h * 0.5f) rad = h * 0.5f;
    if (rad < 0.5f) {
        emit_rect(x, y, w, th, c);
        emit_rect(x, y + h - th, w, th, c);
        emit_rect(x, y, th, h, c);
        emit_rect(x + w - th, y, th, h, c);
        return;
    }
    emit_arc(x + rad,     y + rad,     rad + th * 0.5f, th, VT3_PI,        VT3_PI * 1.5f, c);
    emit_arc(x + w - rad, y + rad,     rad + th * 0.5f, th, VT3_PI * 1.5f, VT3_PI * 2.0f, c);
    emit_arc(x + w - rad, y + h - rad, rad + th * 0.5f, th, 0.0f,          VT3_PI * 0.5f, c);
    emit_arc(x + rad,     y + h - rad, rad + th * 0.5f, th, VT3_PI * 0.5f, VT3_PI,        c);
    emit_line(x + rad, y + th * 0.5f, x + w - rad, y + th * 0.5f, th, c);
    emit_line(x + rad, y + h - th * 0.5f, x + w - rad, y + h - th * 0.5f, th, c);
    emit_line(x + th * 0.5f, y + rad, x + th * 0.5f, y + h - rad, th, c);
    emit_line(x + w - th * 0.5f, y + rad, x + w - th * 0.5f, y + h - rad, th, c);
}

static void emit_glyph(char ch, float x, float y, float px, uint32_t c)
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
                emit_rect(x + (float)start * px, y + (float)row * px, (float)(col - start) * px, px, c);
            } else {
                col++;
            }
        }
    }
}

static void emit_text(const char *s, float x, float y, float px, uint32_t c, int center)
{
    if (center) x -= (float)strlen(s) * 8.0f * px * 0.5f;
    for (; *s; s++, x += 8.0f * px) emit_glyph(*s, x, y, px, c);
}

/* ---- environment, config and visibility ----------------------------------- */

static void load_config(void)
{
    const char *dir = getenv("VT3_SETTINGS_DIR");
    if (!dir || !*dir) dir = ".";
    snprintf(g.cfg_path, sizeof g.cfg_path, "%s/touch.cfg", dir);
    g.cfg_state = vt3_touch_cfg_load(&g.cfg, g.cfg_path);
    if (g.cfg_state == -1) {
        VT3_INFO("touch: no touch.cfg at %s; using the shipped layout", g.cfg_path);
    } else if (g.cfg_state == -2) {
        VT3_CWARN("touch", "touch.cfg is not readable; keeping the last good settings");
    } else {
        VT3_CINFO("touch", "loaded %s (layout %d, profile %d, %s)", g.cfg_path,
                  g.cfg.layout, g.cfg.profile, g.cfg.locked ? "locked" : "unlocked");
    }
}

static void read_env(void)
{
    if (g.inited) return;
    g.inited = 1;
    g.user_scale = 1.0f;
    g.alpha = 1.0f;
    g.color = 1;
    vt3_touch_cfg_defaults(&g.cfg);
    const char *e = getenv("VT3_TOUCH");
#ifdef __ANDROID__
    g.enabled = e ? atoi(e) != 0 : 1;
#else
    g.enabled = e ? atoi(e) != 0 : 0;
#endif
    g.force = getenv("VT3_TOUCH_FORCE") != NULL;
    if ((e = getenv("VT3_TOUCH_SCALE")) != NULL && *e) g.user_scale = clampf((float)atof(e), 0.4f, 2.5f);
    if ((e = getenv("VT3_TOUCH_ALPHA")) != NULL && *e) g.alpha = clampf((float)atof(e), 0.1f, 1.0f);
    if ((e = getenv("VT3_TOUCH_COLOR")) != NULL && *e) g.color = atoi(e) != 0;
    vt3_mode_state ms = { VT3_TOUCH_ANALOG, VT3_TOUCH_ANALOG, 0 };
    g.mode = ms;
    g.rep.dir = VT3_DPAD_NONE;
    vt3_swipe_reset(&g.swipe);
    load_config();
    if (g.enabled) VT3_INFO("touch: on-screen controls enabled (scale %.2f, alpha %.2f, colour %d)",
                            g.user_scale, g.alpha, g.color);
}

/* Fresh state for the next match in this process. The saved layout is re-read by reload(). */
void vt3_touch_reset(void)
{
    release_state();
    vt3_swipe_reset(&g.swipe);
    memset(&g.ptr, 0, sizeof g.ptr);
    g.inited = 0;
    g.have_view = 0;
    g.logical_w = g.logical_h = 0;
    g.draws_at = 0;
    g.draws_max = 0;
    g.guest_menu = 0;
    memset(g.draws_ring, 0, sizeof g.draws_ring);
    free(g.v);
    g.v = NULL;
    g.vcap = g.vn = 0;
    memset(&g.mode, 0, sizeof g.mode);
    g.mode.mode = VT3_TOUCH_ANALOG;
    g.rep.dir = VT3_DPAD_NONE;
}

void vt3_touch_reload(void)
{
    read_env();
    load_config();
    g.draw_mask = 0;
    vt3_touch_release_all();
    VT3_CINFO("touch", "reloaded: %s, layout %d, profile %d", g.enabled ? "on" : "off",
              g.cfg.layout, g.cfg.profile);
}

static int active(void)
{
    read_env();
    return g.enabled;
}

int vt3_touch_enabled(void)
{
    return active();
}

/* The mode the game is in, as far as the controls are concerned. */
static int want_dpad(void)
{
    if (vt3_menu_open()) return 1;        /* the port's own pause menu: a real pause */
    return g.guest_menu;                  /* the game's own menus, from the draw profile */
}

/* ---- layout --------------------------------------------------------------- */

static const vt3_touch_ctl_cfg *ctl_cfg(int ctl) { return vt3_touch_cfg_ctl(&g.cfg, ctl); }

static void layout(void)
{
    float W = (float)g.logical_w, H = (float)g.logical_h;
    float s = H / 1080.0f * g.user_scale;
    g.s = s;

    for (int i = 0; i < VT3_TC_COUNT; i++) {
        const vt3_touch_ctl_cfg *c = ctl_cfg(i);
        ctl_geom *q = &g.geo[i];
        memset(q, 0, sizeof *q);
        g.present[i] = c && !c->removed && c->visible;
        g.opacity[i] = c ? c->opacity : 1.0f;
        float size = c ? c->size : 1.0f;

        /* The built-in positions are the 0.3.0 formulas; a saved x/y overrides them. */
        switch (i) {
        case VT3_TC_STICK:
            q->r = 168.0f * s * size;
            q->cx = c && c->x >= 0.0f ? c->x * W : 200.0f * s;
            q->cy = c && c->y >= 0.0f ? c->y * H : H - 200.0f * s;
            break;
        case VT3_TC_A: case VT3_TC_B: case VT3_TC_X: case VT3_TC_Y: {
            float d = 152.0f * s * size;
            float acx = W - 275.0f * s, acy = H - 250.0f * s;
            static const float ox[4] = { 0.0f, 1.0f, -1.0f, 0.0f };   /* A, B, X, Y */
            static const float oy[4] = { 1.0f, 0.0f, 0.0f, -1.0f };
            int k = i - VT3_TC_A;
            q->r = 74.0f * s * size;
            q->cx = c && c->x >= 0.0f ? c->x * W : acx + ox[k] * d;
            q->cy = c && c->y >= 0.0f ? c->y * H : acy + oy[k] * d;
            break;
        }
        case VT3_TC_LB: case VT3_TC_RB: {
            q->w = 150.0f * s * size;
            q->h = 62.0f * s * size;
            float defx = i == VT3_TC_LB ? 40.0f * s + q->w * 0.5f : W - 40.0f * s - q->w * 0.5f;
            q->cx = c && c->x >= 0.0f ? c->x * W : defx;
            q->cy = c && c->y >= 0.0f ? c->y * H : 34.0f * s + q->h * 0.5f;
            break;
        }
        case VT3_TC_SELECT: case VT3_TC_START: {
            float px = 4.2f * s * size;
            q->h = 66.0f * s * size;
            q->w = (i == VT3_TC_SELECT ? 6.0f : 5.0f) * 8.0f * px + 48.0f * s * size;
            float defx = W * 0.5f + (i == VT3_TC_SELECT ? -140.0f : 140.0f) * s;
            q->cx = c && c->x >= 0.0f ? c->x * W : defx;
            q->cy = c && c->y >= 0.0f ? c->y * H : H - 78.0f * s;
            break;
        }
        case VT3_TC_MENU:
            q->w = 130.0f * s * size;
            q->h = 54.0f * s * size;
            q->cx = c && c->x >= 0.0f ? c->x * W : W * 0.5f;
            q->cy = c && c->y >= 0.0f ? c->y * H : 30.0f * s + q->h * 0.5f;
            break;
        case VT3_TC_SWIPE:
            q->r = 110.0f * s * size;
            q->cx = c && c->x >= 0.0f ? c->x * W : W * 0.5f;
            q->cy = c && c->y >= 0.0f ? c->y * H : H * 0.42f;
            g.present[i] = g.present[i] && g.cfg.swipe_enabled;
            break;
        default:
            break;
        }
    }
}

static int in_rect(float px, float py, float x, float y, float w, float h, float pad)
{
    return px >= x - pad && px <= x + w + pad && py >= y - pad && py <= y + h + pad;
}

/* Which controls a touch at (x, y) is inside, nearest first. Fills candidates/dist. */
static int candidates_at(float x, float y, int *cand, float *dist)
{
    int n = 0;
    const vt3_stick_cfg *prof = vt3_touch_cfg_profile(&g.cfg);

    /* The stick: a circle, or the floating profile's zone (the lower left of the screen). */
    if (g.present[VT3_TC_STICK]) {
        ctl_geom *q = &g.geo[VT3_TC_STICK];
        if (prof && prof->floating) {
            if (x < g.logical_w * 0.55f && y > g.logical_h * 0.35f) {
                cand[n] = VT3_TC_STICK;
                dist[n++] = 0.0f;
            }
        } else {
            float rr = q->r * 1.9f;
            float dx = x - q->cx, dy = y - q->cy;
            float d = sqrtf(dx * dx + dy * dy);
            if (d <= rr) { cand[n] = VT3_TC_STICK; dist[n++] = d; }
        }
    }
    for (int i = VT3_TC_A; i <= VT3_TC_Y; i++) {
        if (!g.present[i]) continue;
        ctl_geom *q = &g.geo[i];
        float rr = q->r * 1.35f;
        float dx = x - q->cx, dy = y - q->cy;
        float d = sqrtf(dx * dx + dy * dy);
        if (d <= rr) { cand[n] = i; dist[n++] = d; }
    }
    for (int i = VT3_TC_LB; i <= VT3_TC_START; i++) {
        if (!g.present[i]) continue;
        ctl_geom *q = &g.geo[i];
        float pad = 14.0f * g.s;
        if (in_rect(x, y, q->cx - q->w * 0.5f, q->cy - q->h * 0.5f, q->w, q->h, pad)) {
            float dx = x - q->cx, dy = y - q->cy;
            cand[n] = i;
            dist[n++] = sqrtf(dx * dx + dy * dy);
        }
    }
    if (g.present[VT3_TC_MENU]) {
        ctl_geom *q = &g.geo[VT3_TC_MENU];
        float pad = 12.0f * g.s;
        if (in_rect(x, y, q->cx - q->w * 0.5f, q->cy - q->h * 0.5f, q->w, q->h, pad)) {
            float dx = x - q->cx, dy = y - q->cy;
            cand[n] = VT3_TC_MENU;
            dist[n++] = sqrtf(dx * dx + dy * dy);
        }
    }
    if (g.present[VT3_TC_SWIPE]) {
        ctl_geom *q = &g.geo[VT3_TC_SWIPE];
        float dx = x - q->cx, dy = y - q->cy;
        float d = sqrtf(dx * dx + dy * dy);
        if (d <= q->r) { cand[n] = VT3_TC_SWIPE; dist[n++] = d; }
    }
    return n;
}

static uint32_t owned_mask(void)
{
    uint32_t m = 0;
    for (int i = 0; i < TOUCH_MAX_PTR; i++)
        if (g.ptr[i].used && g.ptr[i].ctl >= 0) m |= 1u << g.ptr[i].ctl;
    return m;
}

/* ---- input -> game state -------------------------------------------------- */

static uint32_t now_ms(void)
{
    /* The renderer's own frame clock: monotonic, and already used by the overlay. */
    static uint32_t base;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t t = (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
    if (!base) base = t;
    return t;
}

/* VT3_TOUCH_DEBUG=1 traces every change of what the controls are holding. */
static void log_state(uint32_t mask, float lx, float ly, int mode)
{
    static int dbg = -1;
    static uint32_t last_mask;
    static float last_lx, last_ly;
    static int last_mode = -1;
    if (dbg < 0) dbg = getenv("VT3_TOUCH_DEBUG") != NULL;
    if (!dbg) return;
    if (mask == last_mask && lx == last_lx && ly == last_ly && mode == last_mode) return;
    last_mask = mask; last_lx = lx; last_ly = ly; last_mode = mode;
    int held = 0;
    for (int i = 0; i < TOUCH_MAX_PTR; i++) if (g.ptr[i].used) held++;
    VT3_INFO("touch: %s buttons %08x stick %+.2f,%+.2f (%d pointers)",
             mode == VT3_TOUCH_DPAD ? "dpad" : "analog", mask, lx, ly, held);
}

/* Send the d-pad bit for a direction for one short pulse, so a menu sees a real press and release. */
static void pulse_dpad(int dir, uint32_t now)
{
    uint32_t bit = dir == VT3_DPAD_UP ? VT3_BTN_UP : dir == VT3_DPAD_DOWN ? VT3_BTN_DOWN
                 : dir == VT3_DPAD_LEFT ? VT3_BTN_LEFT : dir == VT3_DPAD_RIGHT ? VT3_BTN_RIGHT : 0;
    if (!bit) return;
    g.pulse_mask |= bit;
    g.dpad_until_ms = now + DPAD_PULSE_MS;
}

/* Push whatever the controls currently hold into the game, changing only what differs. */
static void apply_state(void)
{
    uint32_t now = now_ms();
    uint32_t mask = 0;
    float lx = 0.0f, ly = 0.0f;
    int stick = 0;
    float kx = g.geo[VT3_TC_STICK].cx, ky = g.geo[VT3_TC_STICK].cy;
    const vt3_stick_cfg *prof = vt3_touch_cfg_profile(&g.cfg);

    for (int i = 0; i < TOUCH_MAX_PTR; i++) {
        touch_ptr *p = &g.ptr[i];
        if (!p->used || p->ctl < 0) continue;
        if (p->ctl == VT3_TC_STICK) {
            stick = 1;
            float cx = p->ax, cy = p->ay;          /* floating profile moves the centre */
            float r = g.geo[VT3_TC_STICK].r;
            float dx = p->lx - cx, dy = p->ly - cy;
            float d = sqrtf(dx * dx + dy * dy);
            if (d > r && d > 0.0f) { dx = dx * r / d; dy = dy * r / d; }
            kx = cx + dx;
            ky = cy + dy;
            if (g.mode.mode == VT3_TOUCH_ANALOG) {
                vt3_stick_apply(prof, dx, dy, r, &lx, &ly);
            } else {
                /* Digital: a direction, repeated with menu timing. */
                float ax = r > 0.0f ? dx / r : 0.0f;
                float ay = r > 0.0f ? dy / r : 0.0f;
                int dir = VT3_DPAD_NONE;
                if (fabsf(ax) > 0.35f || fabsf(ay) > 0.35f)
                    dir = fabsf(ax) > fabsf(ay) ? (ax < 0 ? VT3_DPAD_LEFT : VT3_DPAD_RIGHT)
                                                : (ay < 0 ? VT3_DPAD_UP : VT3_DPAD_DOWN);
                vt3_repeat_timing t = { 400, 120 };
                if (vt3_repeat_update(&g.rep, dir, now, &t)) pulse_dpad(dir, now);
                if (dir == VT3_DPAD_NONE) g.rep.dir = VT3_DPAD_NONE;
                if (dir != g.last_dir) {
                    VT3_CDEBUG("touch", "menu direction %d", dir);
                    g.last_dir = dir;
                }
            }
        } else if (p->ctl == VT3_TC_SWIPE) {
            /* The action was pressed when the direction locked (see vt3_touch_pointer); holding the
             * finger keeps it pressed, which is exactly what holding the button does. */
        } else {
            mask |= vt3_touch_button_mask(p->ctl);
        }
    }

    /* The swipe action, while its gesture is live. */
    if (g.swipe.active && g.swipe.fired && g.swipe.dir != VT3_DPAD_NONE) {
        int c = vt3_touch_swipe_ctl(g.cfg.swipe_map, g.swipe.dir);
        mask |= vt3_touch_button_mask(c);
    }

    g.knob_x = kx;
    g.knob_y = ky;
    g.stick_held = stick;

    /* A pulse that has run its course is released here. */
    if (g.pulse_mask && (int32_t)(now - g.dpad_until_ms) >= 0) {
        uint32_t off = g.pulse_mask;
        g.pulse_mask = 0;
        vt3_input_set_button(off, 0);
        vt3_input_set_pad_button(off, 0);
    }
    mask |= g.pulse_mask;

    g.draw_mask = mask;
    log_state(mask, lx, ly, g.mode.mode);

    uint32_t on = mask & ~g.applied_mask;
    uint32_t off = g.applied_mask & ~mask;
    if (on)  { vt3_input_set_button(on, 1);  vt3_input_set_pad_button(on, 1); }
    if (off) { vt3_input_set_button(off, 0); vt3_input_set_pad_button(off, 0); }
    g.applied_mask = mask;

    if (lx != g.applied_lx || ly != g.applied_ly) {
        vt3_input_set_axis(VT3_AXIS_LX, lx);
        vt3_input_set_axis(VT3_AXIS_LY, ly);
        g.applied_lx = lx;
        g.applied_ly = ly;
    }
}

/* Drop everything the controls were holding (hidden, focus lost, mode change, window gone). */
static void release_state(void)
{
    for (int i = 0; i < TOUCH_MAX_PTR; i++) g.ptr[i].used = 0;
    g.draw_mask = 0;
    g.stick_held = 0;
    g.pulse_mask = 0;
    g.rep.dir = VT3_DPAD_NONE;
    g.last_dir = VT3_DPAD_NONE;
    vt3_swipe_reset(&g.swipe);
    if (g.applied_mask) {
        vt3_input_set_button(g.applied_mask, 0);
        vt3_input_set_pad_button(g.applied_mask, 0);
        g.applied_mask = 0;
    }
    if (g.applied_lx != 0.0f || g.applied_ly != 0.0f) {
        vt3_input_set_axis(VT3_AXIS_LX, 0.0f);
        vt3_input_set_axis(VT3_AXIS_LY, 0.0f);
        g.applied_lx = g.applied_ly = 0.0f;
    }
}

void vt3_touch_pointer(int id, int down, float x, float y)
{
    if (!vt3_touch_enabled()) return;
    float lx, ly;
    vt3_touch_rect dst = { g.dst.x, g.dst.y, g.dst.w, g.dst.h };
    vt3_touch_map_point(x, y, dst, g.logical_w, g.logical_h, &lx, &ly);

    touch_ptr *p = NULL;
    for (int i = 0; i < TOUCH_MAX_PTR; i++) {
        if (g.ptr[i].used && g.ptr[i].id == id) { p = &g.ptr[i]; break; }
    }
    if (!p) {
        if (!down) return;
        for (int i = 0; i < TOUCH_MAX_PTR; i++) {
            if (!g.ptr[i].used) { p = &g.ptr[i]; break; }
        }
        if (!p) return;
        int cand[VT3_TC_COUNT];
        float dist[VT3_TC_COUNT];
        int n = candidates_at(lx, ly, cand, dist);
        int ctl = vt3_touch_pick(cand, dist, n, owned_mask());
        if (ctl < 0) return;                       /* outside the controls, or already held */
        p->used = 1;
        p->id = id;
        p->ctl = ctl;
        p->lx = lx;
        p->ly = ly;
        const vt3_stick_cfg *prof = vt3_touch_cfg_profile(&g.cfg);
        if (ctl == VT3_TC_STICK) {
            /* Floating: the stick's centre is where the thumb landed, inside the control zone. */
            if (prof && prof->floating) {
                p->ax = lx;
                p->ay = ly;
            } else {
                p->ax = g.geo[VT3_TC_STICK].cx;
                p->ay = g.geo[VT3_TC_STICK].cy;
            }
        }
        if (ctl == VT3_TC_MENU) {
            /* The same pause menu Start reaches, from a touch: no new menu system. */
            p->used = 0;
            p->ctl = -1;
            VT3_CINFO("touch", "menu button pressed");
            vt3_touch_release_all();
            vt3_menu_set_open(1);
            return;
        }
        if (ctl == VT3_TC_SWIPE) {
            if (!vt3_swipe_begin(&g.swipe, id, lx, ly, now_ms())) { p->used = 0; p->ctl = -1; return; }
            VT3_CDEBUG("touch", "swipe gesture started (pointer %d)", id);
        }
    }
    if (down) {
        p->lx = lx;
        p->ly = ly;
        if (p->ctl == VT3_TC_SWIPE) {
            int dir = VT3_DPAD_NONE;
            float min_d = g.geo[VT3_TC_SWIPE].r * g.cfg.swipe_min_dist * 2.0f;
            if (vt3_swipe_move(&g.swipe, id, lx, ly, min_d, &dir)) {
                g.swipe.fired = 1;
                int c = vt3_touch_swipe_ctl(g.cfg.swipe_map, dir);
                uint32_t bit = vt3_touch_button_mask(c);
                if (bit) { vt3_input_set_button(bit, 1); vt3_input_set_pad_button(bit, 1); }
                VT3_CINFO("touch", "swipe %d -> control %d", dir, c);
            }
        }
    } else {
        if (p->ctl == VT3_TC_SWIPE) {
            int dir = VT3_DPAD_NONE, fired = 0;
            vt3_swipe_end(&g.swipe, id, &dir, &fired);
            int c = vt3_touch_swipe_ctl(g.cfg.swipe_map, dir);
            uint32_t bit = vt3_touch_button_mask(c);
            if (fired && bit) { vt3_input_set_button(bit, 0); vt3_input_set_pad_button(bit, 0); }
            if (!fired) VT3_CDEBUG("touch", "swipe cancelled");
        }
        p->used = 0;
        p->ctl = -1;
    }
    apply_state();
}

void vt3_touch_release_all(void)
{
    release_state();
}

/* The guest's draw profile: a menu draws a fraction of what a rally does. The ring keeps the
 * session's own high-water mark, so no fixed threshold has to be tuned per device. */
void vt3_touch_note_draws(int draws)
{
    if (draws < 0) draws = 0;
    g.draws_ring[g.draws_at] = (unsigned)draws;
    g.draws_at = (g.draws_at + 1) % 64;
    if ((unsigned)draws > g.draws_max) g.draws_max = (unsigned)draws;
    /* Forget the high-water mark slowly, so one heavy frame does not keep the controls in match
     * mode through an entire menu. */
    if ((g.draws_at & 15) == 0 && g.draws_max > 8) g.draws_max = g.draws_max * 15u / 16u;

    unsigned sum = 0;
    for (int i = 0; i < 64; i++) sum += g.draws_ring[i];
    unsigned avg = sum / 64u;
    int menu = g.draws_max > 12 && avg * 4u < g.draws_max;
    if (menu != g.guest_menu) {
        g.guest_menu = menu;
        VT3_CINFO("touch", "guest looks like a %s (avg %u draws, peak %u)",
                  menu ? "menu" : "match", avg, g.draws_max);
    }
}

/* ---- drawing -------------------------------------------------------------- */

static void draw_disc_shape(float cx, float cy, float r, uint32_t fill, uint32_t stroke, float th)
{
    for (int pass = 0; pass < 2; pass++) {
        g.shadow = pass == 0;
        emit_disc(cx, cy, r, fill);
        emit_ring(cx, cy, r, th, stroke);
    }
    g.shadow = 0;
}

static void draw_pill(float cx, float cy, float w, float h, const char *label, float px,
                      uint32_t fill, uint32_t stroke, uint32_t text)
{
    float x = cx - w * 0.5f, y = cy - h * 0.5f;
    float rad = h * 0.5f;
    float th = 3.0f * g.s;
    if (th < 1.5f) th = 1.5f;
    for (int pass = 0; pass < 2; pass++) {
        g.shadow = pass == 0;
        emit_round_rect(x, y, w, h, rad, h, fill);
        emit_round_rect(x, y, w, h, rad, th, stroke);
        emit_text(label, cx, cy - 4.0f * px, px, text, 1);
    }
    g.shadow = 0;
}

static void draw_controls(void)
{
    float s = g.s;
    float th = 3.2f * s;
    if (th < 1.6f) th = 1.6f;
    g_sh_x = 0.0f;
    g_sh_y = 1.5f * s;

    uint32_t stroke = rgba(233, 238, 247, 0.72f);
    uint32_t fill   = rgba(255, 255, 255, 0.10f);
    uint32_t fill_p = rgba(255, 255, 255, 0.30f);
    uint32_t text   = rgba(238, 242, 250, 0.94f);

    if (g.present[VT3_TC_STICK]) {
        ctl_geom *q = &g.geo[VT3_TC_STICK];
        int sp = g.stick_held;
        for (int pass = 0; pass < 2; pass++) {
            g.shadow = pass == 0;
            emit_disc(q->cx, q->cy, q->r - th * 0.5f, sp ? fill_p : fill);
            emit_ring(q->cx, q->cy, q->r, th, stroke);
        }
        g.shadow = 0;
        float kr = 70.0f * s * (q->r / (168.0f * s > 0.0f ? 168.0f * s : 1.0f));
        for (int pass = 0; pass < 2; pass++) {
            g.shadow = pass == 0;
            emit_disc(g.knob_x, g.knob_y, kr, rgba(255, 255, 255, sp ? 0.30f : 0.16f));
            emit_ring(g.knob_x, g.knob_y, kr, th * 0.85f, stroke);
        }
        g.shadow = 0;
    }

    static const char *ab_label = "ABXY";
    for (int i = VT3_TC_A; i <= VT3_TC_Y; i++) {
        if (!g.present[i]) continue;
        ctl_geom *q = &g.geo[i];
        int pressed = (g.draw_mask & vt3_touch_button_mask(i)) != 0;
        float r = q->r * (pressed ? 1.06f : 1.0f);
        draw_disc_shape(q->cx, q->cy, r, accent(i, (pressed ? 0.44f : 0.16f) * g.opacity[i]),
                        accent(i, (pressed ? 1.0f : 0.88f) * g.opacity[i]), th);
        float bpx = q->r * 0.155f;
        for (int pass = 0; pass < 2; pass++) {
            g.shadow = pass == 0;
            emit_glyph(ab_label[i - VT3_TC_A], q->cx - 4.0f * bpx, q->cy - 4.0f * bpx, bpx, text);
        }
        g.shadow = 0;
    }

    for (int i = VT3_TC_LB; i <= VT3_TC_START; i++) {
        if (!g.present[i]) continue;
        ctl_geom *q = &g.geo[i];
        int pressed = (g.draw_mask & vt3_touch_button_mask(i)) != 0;
        const char *label = i == VT3_TC_LB ? "LB" : i == VT3_TC_RB ? "RB"
                          : i == VT3_TC_SELECT ? "SELECT" : "START";
        float px = (i == VT3_TC_SELECT || i == VT3_TC_START ? 4.2f : 5.0f) * s * (q->h / (66.0f * s > 0.0f ? 66.0f * s : 1.0f));
        draw_pill(q->cx, q->cy, q->w, q->h, label, px,
                  pressed ? fill_p : fill, stroke, text);
    }

    if (g.present[VT3_TC_MENU]) {
        ctl_geom *q = &g.geo[VT3_TC_MENU];
        float px = 4.4f * s * (q->h / (54.0f * s > 0.0f ? 54.0f * s : 1.0f));
        draw_pill(q->cx, q->cy, q->w, q->h, "MENU", px, fill, stroke, text);
    }

    if (g.present[VT3_TC_SWIPE]) {
        ctl_geom *q = &g.geo[VT3_TC_SWIPE];
        float a = g.opacity[VT3_TC_SWIPE];
        int locked = vt3_swipe_locked(&g.swipe);
        for (int pass = 0; pass < 2; pass++) {
            g.shadow = pass == 0;
            emit_disc(q->cx, q->cy, q->r, rgba(255, 255, 255, (locked ? 0.24f : 0.10f) * a));
            emit_ring(q->cx, q->cy, q->r, th, rgba(233, 238, 247, 0.66f * a));
        }
        g.shadow = 0;
        if (g.cfg.swipe_labels) {
            static const char *lbl[4] = { "A", "B", "X", "Y" };
            static const char *arrow[4] = { "UP", "DN", "LT", "RT" };
            float px = 1.6f * s;
            for (int d = 0; d < 4; d++) {
                int c = vt3_touch_swipe_ctl(g.cfg.swipe_map, d);
                if (c < VT3_TC_A || c > VT3_TC_Y) continue;
                float ox = d == VT3_DPAD_LEFT ? -0.62f : d == VT3_DPAD_RIGHT ? 0.62f : 0.0f;
                float oy = d == VT3_DPAD_UP ? -0.62f : d == VT3_DPAD_DOWN ? 0.62f : 0.0f;
                char line[16];
                snprintf(line, sizeof line, "%s %s", arrow[d], lbl[c - VT3_TC_A < 4 ? c - VT3_TC_A : 0]);
                emit_text(line, q->cx + ox * q->r, q->cy + oy * q->r - 4.0f * px, px,
                          rgba(238, 242, 250, 0.80f * a), 1);
            }
        }
    }
}

void vt3_touch_frame(int drawable_w, int drawable_h)
{
    if (!active()) {
        release_state();
        return;
    }

    int lw = 0, lh = 0;
    vt3_gfx_logical_size(&lw, &lh);
    if (lw <= 0 || lh <= 0) return;
    g.logical_w = lw;
    g.logical_h = lh;
    vt3_gfx_view_rect(drawable_w, drawable_h, &g.dst);
    g.have_view = 1;

    layout();

    /* Mode first: a change releases whatever the old mode held, so nothing sticks. */
    int changed = 0;
    int before = g.mode.mode;
    int now_mode = vt3_mode_update(&g.mode, want_dpad(), now_ms(), MENU_DWELL_MS, &changed);
    if (changed) {
        VT3_CINFO("touch", "stick mode %s -> %s", before == VT3_TOUCH_DPAD ? "d-pad" : "analog",
                  now_mode == VT3_TOUCH_DPAD ? "d-pad" : "analog");
        release_state();
    }

    apply_state();

    vt3_gfx_rt_bind(0);
    vt3_rect none = { 0, 0, 0, 0 };
    vt3_gfx_set_scissor(0, none);

    g.vn = 0;
    g.shadow = 0;
    draw_controls();
    if (g.vn) vt3_gfx_draw(VT3_PRIM_TRIANGLELIST, g.v, g.vn, 0);
}

int vt3_touch_mode(void)
{
    return g.mode.mode;
}

int vt3_touch_guest_menu(void)
{
    return g.guest_menu;
}
