/*
 * The parts of the on-screen controls that can be decided without a renderer or a device: the
 * analog stick transform, the analog/d-pad mode switch, menu navigation repeat timing, gesture
 * recognition and touch-to-control ownership.
 *
 * They live apart from touch_controls.c so tools/touchtest can exercise them on a workstation:
 * every rule below has a host test, and the rendering code only calls in.
 */
#ifndef VT3_TOUCH_CORE_H
#define VT3_TOUCH_CORE_H

#include "vt3/vt3_input.h"

#include <stdint.h>

/* ---- stick response -------------------------------------------------------- */

/* A stick profile. The Default profile is the transform the port shipped with: a 14% radial dead
 * zone, linear response and full travel at the drawn radius. */
typedef struct {
    float deadzone;      /* 0..0.6, fraction of the radius that reads as centred */
    float sensitivity;   /* 0.5..2, scales the output magnitude */
    float range;         /* 0.5..1.5, fraction of the radius that reaches full output */
    int   progressive;   /* 0 = linear, 1 = output grows with the square of the displacement */
    int   floating;      /* 1 = the stick's centre follows where the thumb landed */
    int   smooth_return; /* 1 = the drawn knob eases back to centre (visual only) */
    int   invert_x, invert_y;
    float sens_x, sens_y;/* 0.5..2, per-axis scale on top of sensitivity */
} vt3_stick_cfg;

/* Maps a displacement (dx, dy) in logical pixels inside a stick of radius `radius` to the axis
 * values the game reads. The Default profile reproduces the original code exactly:
 *
 *     clamp the offset to the radius, normalise by it, subtract the 0.14 dead zone,
 *     rescale the remainder to 0..1.
 *
 * out_x/out_y are -1..1, +x right, +y down (the DirectInput sign the shim already uses). */
void vt3_stick_apply(const vt3_stick_cfg *c, float dx, float dy, float radius,
                     float *out_x, float *out_y);

/* The transform as shipped in 0.3.0, kept verbatim so a regression test can compare against it. */
void vt3_stick_apply_original(float dx, float dy, float radius, float *out_x, float *out_y);

/* ---- analog / d-pad switching --------------------------------------------- */

enum { VT3_TOUCH_ANALOG = 0, VT3_TOUCH_DPAD = 1 };

typedef struct {
    int      mode;            /* VT3_TOUCH_ANALOG or VT3_TOUCH_DPAD */
    int      want;            /* the candidate the caller last asked for */
    uint32_t since_ms;        /* when the candidate first differed from the mode */
} vt3_mode_state;

/* Feeds the wanted mode in; returns the mode to use now. A change only happens once the candidate
 * has held for dwell_ms, so a menu that flashes up for one frame cannot flip the stick. On a
 * change *changed is set to 1 and the caller must release whatever the old mode was holding. */
int vt3_mode_update(vt3_mode_state *s, int want_dpad, uint32_t now_ms, uint32_t dwell_ms, int *changed);

/* ---- menu navigation repeat ----------------------------------------------- */

enum { VT3_DPAD_NONE = -1, VT3_DPAD_UP = 0, VT3_DPAD_DOWN = 1, VT3_DPAD_LEFT = 2, VT3_DPAD_RIGHT = 3 };

typedef struct {
    uint32_t first_delay_ms;  /* how long the first press is held before repeating (400) */
    uint32_t repeat_ms;       /* the gap between repeats after that (120) */
} vt3_repeat_timing;

typedef struct {
    int      dir;             /* VT3_DPAD_* currently repeating, or VT3_DPAD_NONE */
    int      fired;           /* the direction has been sent at least once */
    uint32_t next_ms;         /* when the next repeat is due */
} vt3_repeat_state;

/* Menu navigation from a direction. The first press is emitted at once (returns 1), then repeats
 * after first_delay_ms and every repeat_ms after that, so a tap moves one step and a hold does not
 * race through the list. Returns 1 when the direction should be pulsed this frame, 0 otherwise. */
int vt3_repeat_update(vt3_repeat_state *s, int dir, uint32_t now_ms, const vt3_repeat_timing *t);

/* ---- one-button swipe ------------------------------------------------------ */

typedef struct {
    int   active;
    int   id;                 /* pointer id that owns the gesture, -1 when idle */
    float sx, sy;             /* where the gesture started */
    float lx, ly;
    int   dir;                /* VT3_DPAD_* once locked, VT3_DPAD_NONE before */
    int   fired;              /* the action has been sent for this gesture */
    uint32_t start_ms;
} vt3_swipe_state;

void vt3_swipe_reset(vt3_swipe_state *s);

/* A finger landed on the swipe pad. Returns 1 when the gesture is taken (the first finger only:
 * a second finger on the pad is ignored, so two thumbs cannot fight over one action). */
int vt3_swipe_begin(vt3_swipe_state *s, int id, float x, float y, uint32_t now_ms);

/* Movement. Once the displacement passes min_dist the direction is locked and reported in *dir
 * (VT3_DPAD_*); moving back towards the start does not change it. Returns 1 the frame the
 * direction locks. */
int vt3_swipe_move(vt3_swipe_state *s, int id, float x, float y, float min_dist, int *dir);

/* The finger lifted. *dir is the locked direction (VT3_DPAD_NONE when the swipe never travelled
 * far enough) and *fired says whether the action was already sent. */
void vt3_swipe_end(vt3_swipe_state *s, int id, int *dir, int *fired);

/* 1 while a gesture is in progress and its direction is locked (for the label and charge ring). */
int vt3_swipe_locked(const vt3_swipe_state *s);

/* ---- coordinates and button mapping --------------------------------------- */

typedef struct { int x, y, w, h; } vt3_touch_rect;

/* Maps a point in drawable (window) pixels into the renderer's logical pixels through the present
 * letterbox, which is what makes a control hit where it is drawn on any aspect ratio. */
void vt3_touch_map_point(float dx, float dy, vt3_touch_rect dst, int logical_w, int logical_h,
                         float *lx, float *ly);

/* The game button a control sends. This is the original mapping: A->A, B->B, X->X, Y->Y, LB->LB,
 * RB->RB, Select->BACK, Start->START. It lives here so a host test can assert it, and so the
 * renderer has no second copy to drift from. */
uint32_t vt3_touch_button_mask(int ctl);

/* ---- touch ownership ------------------------------------------------------- */

/* Picks which control a new touch belongs to. candidates[i] is a control index or -1; dist[i] its
 * distance from the touch centre. Controls already owned by another finger are skipped, so a
 * second finger never steals a held button, and of the rest the nearest wins: overlapping zones
 * resolve predictably. Returns the control index or -1. */
int vt3_touch_pick(const int *candidates, const float *dist, int n, uint32_t owned_mask);

/* The control index a swipe direction maps to, from the saved swipe.map.* table. */
int vt3_touch_swipe_ctl(const int map[4], int dir);

#endif /* VT3_TOUCH_CORE_H */
