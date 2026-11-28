/*
 * The touch overlay's menu mode: the compact pad drawn while the game is showing one of its own
 * menus. A d-pad cross, A and B, and the port's MENU pill -- and nothing else.
 *
 * The geometry lives here, out of touch_controls.c, for the same reason the quick menu's does: the
 * overlay draws these numbers and hit-tests these numbers, so a control cannot be drawn somewhere
 * it cannot be tapped, and tools/touchtest can check that on a workstation instead of a phone.
 */
#ifndef VT3_TOUCH_MENU_LAYOUT_H
#define VT3_TOUCH_MENU_LAYOUT_H

#include "vt3/vt3_touch_config.h"
#include "vt3/vt3_touch_core.h"

/* Entries in the menu pad: the cross, B, A and the MENU pill. */
#define VT3_TML_ENTRIES 4

enum {
    VT3_TML_CROSS = 0,      /* the stick, drawn as a cross */
    VT3_TML_B,
    VT3_TML_A,
    VT3_TML_MENU
};

typedef enum {
    VT3_TML_CIRCLE = 0,     /* the cross and the face buttons */
    VT3_TML_PILL            /* the MENU pill */
} vt3_tml_kind;

/* One drawn control. Circle entries use cx/cy/r; pill entries use cx/cy/w/h. */
typedef struct {
    vt3_tml_kind kind;
    int   ctl;              /* the VT3_TC_* control this entry drives */
    float cx, cy;
    float r;
    float w, h;
} vt3_tml_ctl;

/* Fills out[VT3_TML_ENTRIES] for a logical frame of lw x lh, with the overlay's scale s (frame
 * height over 1080, times the user's size setting). The layout is fixed: the cross low in the left
 * corner where the thumb rests, A and B side by side in the right corner, and MENU in the top strip
 * above a centred list. */
void vt3_touch_menu_layout(float lw, float lh, float s, vt3_tml_ctl *out);

/* The entry a logical point hits, or -1. Applies the same pick tolerance as the match hit test
 * (VT3_TOUCH_PICK_*), so anything drawn can be hit; the nearest of overlapping picks wins. */
int vt3_touch_menu_hit(const vt3_tml_ctl *ctls, int n, float x, float y, float s);

#endif /* VT3_TOUCH_MENU_LAYOUT_H */
