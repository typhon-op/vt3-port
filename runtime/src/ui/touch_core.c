/*
 * Pure touch-control logic (see vt3/vt3_touch_core.h). No renderer, no platform, no allocation:
 * tools/touchtest drives every function here directly.
 */
#include "vt3/vt3_touch_core.h"

#include <math.h>

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* The 0.3.0 transform, verbatim: the regression test compares the Default profile against it. */
void vt3_stick_apply_original(float dx, float dy, float radius, float *out_x, float *out_y)
{
    float lx = 0.0f, ly = 0.0f;
    if (radius > 0.0f) {
        float d = sqrtf(dx * dx + dy * dy);
        if (d > radius && d > 0.0f) { dx = dx * radius / d; dy = dy * radius / d; d = radius; }
        float ax = dx / radius;
        float ay = dy / radius;
        const float dz = 0.14f;
        float mag = sqrtf(ax * ax + ay * ay);
        if (mag <= dz) { ax = 0.0f; ay = 0.0f; }
        else { float k = (mag - dz) / (1.0f - dz) / mag; ax *= k; ay *= k; }
        lx = clampf(ax, -1.0f, 1.0f);
        ly = clampf(ay, -1.0f, 1.0f);
    }
    *out_x = lx;
    *out_y = ly;
}

void vt3_stick_apply(const vt3_stick_cfg *c, float dx, float dy, float radius,
                     float *out_x, float *out_y)
{
    float lx = 0.0f, ly = 0.0f;
    if (c && radius > 0.0f) {
        /* Range is the fraction of the drawn radius that reaches full output. */
        float range = c->range > 0.05f ? c->range : 1.0f;
        float lim = radius * range;
        float d = sqrtf(dx * dx + dy * dy);
        if (d > lim && d > 0.0f) { dx = dx * lim / d; dy = dy * lim / d; }
        float ax = dx / lim;
        float ay = dy / lim;

        float dz = clampf(c->deadzone, 0.0f, 0.6f);
        float mag = sqrtf(ax * ax + ay * ay);
        if (mag <= dz) {
            ax = 0.0f;
            ay = 0.0f;
        } else {
            float k = (mag - dz) / (1.0f - dz) / mag;
            ax *= k;
            ay *= k;
        }

        /* Progressive: the output grows with the square of the displacement, so small movements
         * are gentler while full travel still reaches 1. With a factor of 1 the arithmetic is the
         * original, unchanged. */
        if (c->progressive) {
            float m = sqrtf(ax * ax + ay * ay);
            ax *= m;
            ay *= m;
        }

        ax *= c->sensitivity * c->sens_x;
        ay *= c->sensitivity * c->sens_y;
        if (c->invert_x) ax = -ax;
        if (c->invert_y) ay = -ay;
        lx = clampf(ax, -1.0f, 1.0f);
        ly = clampf(ay, -1.0f, 1.0f);
    }
    *out_x = lx;
    *out_y = ly;
}

int vt3_mode_update(vt3_mode_state *s, int want_dpad, uint32_t now_ms, uint32_t dwell_ms, int *changed)
{
    if (changed) *changed = 0;
    if (!s) return VT3_TOUCH_ANALOG;
    int want = want_dpad ? VT3_TOUCH_DPAD : VT3_TOUCH_ANALOG;
    if (want == s->mode) {
        s->want = want;
        return s->mode;
    }
    if (want != s->want) {
        /* A different request starts the dwell again, so a menu that flickers cannot switch. */
        s->want = want;
        s->since_ms = now_ms;
        return s->mode;
    }
    if ((uint32_t)(now_ms - s->since_ms) >= dwell_ms) {
        s->mode = want;
        if (changed) *changed = 1;
    }
    return s->mode;
}

int vt3_repeat_update(vt3_repeat_state *s, int dir, uint32_t now_ms, const vt3_repeat_timing *t)
{
    if (!s) return 0;
    uint32_t first = t && t->first_delay_ms ? t->first_delay_ms : 400u;
    uint32_t rep = t && t->repeat_ms ? t->repeat_ms : 120u;

    if (dir != s->dir) {
        s->dir = dir;
        s->fired = dir != VT3_DPAD_NONE;
        s->next_ms = now_ms + first;
        return dir != VT3_DPAD_NONE;         /* the first step is immediate */
    }
    if (dir == VT3_DPAD_NONE) return 0;
    if ((int32_t)(now_ms - s->next_ms) >= 0) {
        s->next_ms = now_ms + rep;
        s->fired = 1;
        return 1;
    }
    return 0;
}

void vt3_swipe_reset(vt3_swipe_state *s)
{
    if (!s) return;
    s->active = 0;
    s->id = -1;
    s->sx = s->sy = s->lx = s->ly = 0.0f;
    s->dir = VT3_DPAD_NONE;
    s->fired = 0;
    s->start_ms = 0;
}

int vt3_swipe_begin(vt3_swipe_state *s, int id, float x, float y, uint32_t now_ms)
{
    if (!s || s->active) return 0;
    s->active = 1;
    s->id = id;
    s->sx = s->lx = x;
    s->sy = s->ly = y;
    s->dir = VT3_DPAD_NONE;
    s->fired = 0;
    s->start_ms = now_ms;
    return 1;
}

int vt3_swipe_move(vt3_swipe_state *s, int id, float x, float y, float min_dist, int *dir)
{
    if (dir) *dir = VT3_DPAD_NONE;
    if (!s || !s->active || s->id != id) return 0;
    s->lx = x;
    s->ly = y;
    if (s->dir != VT3_DPAD_NONE) { if (dir) *dir = s->dir; return 0; }
    float dx = x - s->sx, dy = y - s->sy;
    float ax = fabsf(dx), ay = fabsf(dy);
    if ((ax > ay ? ax : ay) < min_dist) return 0;
    /* The dominant axis locks the action; from here it cannot change for this gesture. */
    s->dir = ax > ay ? (dx < 0.0f ? VT3_DPAD_LEFT : VT3_DPAD_RIGHT)
                     : (dy < 0.0f ? VT3_DPAD_UP : VT3_DPAD_DOWN);
    if (dir) *dir = s->dir;
    return 1;
}

void vt3_swipe_end(vt3_swipe_state *s, int id, int *dir, int *fired)
{
    if (dir) *dir = VT3_DPAD_NONE;
    if (fired) *fired = 0;
    if (!s || !s->active || s->id != id) return;
    if (dir) *dir = s->dir;
    if (fired) *fired = s->fired;
    vt3_swipe_reset(s);
}

int vt3_swipe_locked(const vt3_swipe_state *s)
{
    return s && s->active && s->dir != VT3_DPAD_NONE;
}

void vt3_touch_map_point(float dx, float dy, vt3_touch_rect dst, int logical_w, int logical_h,
                         float *lx, float *ly)
{
    if (dst.w <= 0 || dst.h <= 0 || logical_w <= 0 || logical_h <= 0) {
        *lx = dx; *ly = dy;
        return;
    }
    *lx = (dx - (float)dst.x) * (float)logical_w / (float)dst.w;
    *ly = (dy - (float)dst.y) * (float)logical_h / (float)dst.h;
}

uint32_t vt3_touch_button_mask(int ctl)
{
    /* VT3_TC_* order: stick, a, b, x, y, lb, rb, select, start, swipe. */
    switch (ctl) {
    case 1: return VT3_BTN_A;
    case 2: return VT3_BTN_B;
    case 3: return VT3_BTN_X;
    case 4: return VT3_BTN_Y;
    case 5: return VT3_BTN_LB;
    case 6: return VT3_BTN_RB;
    case 7: return VT3_BTN_BACK;
    case 8: return VT3_BTN_START;
    default: return 0;
    }
}

int vt3_touch_swipe_ctl(const int map[4], int dir)
{
    if (!map || dir < 0 || dir > 3) return -1;
    return map[dir];
}

int vt3_touch_pick(const int *candidates, const float *dist, int n, uint32_t owned_mask)
{
    int best = -1;
    float best_d = 0.0f;
    for (int i = 0; i < n; i++) {
        int c = candidates[i];
        if (c < 0) continue;
        if (owned_mask & (1u << c)) continue;      /* another finger already holds it */
        if (best < 0 || dist[i] < best_d) { best = c; best_d = dist[i]; }
    }
    return best;
}
