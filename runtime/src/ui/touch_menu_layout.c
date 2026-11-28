#include "vt3/vt3_touch_menu_layout.h"

#include <math.h>
#include <stddef.h>

/* The drawn sizes, at a 1080-high frame. Small on purpose: this pad exists so the game's own menu
 * stays readable behind it. */
#define CROSS_R    138.0f
#define CROSS_X    240.0f
#define CROSS_Y    260.0f       /* up from the bottom edge */
#define FACE_R      72.0f
#define FACE_X     220.0f      /* in from the right edge */
#define FACE_Y     270.0f      /* up from the bottom edge */
#define FACE_GAP   180.0f      /* between B and A */
#define PILL_W     130.0f
#define PILL_H      54.0f
#define PILL_TOP    30.0f      /* down from the top edge */

void vt3_touch_menu_layout(float lw, float lh, float s, vt3_tml_ctl *out)
{
    if (!out) return;
    if (s <= 0.0f) s = 1.0f;

    out[VT3_TML_CROSS].kind = VT3_TML_CIRCLE;
    out[VT3_TML_CROSS].ctl = VT3_TC_STICK;
    out[VT3_TML_CROSS].cx = CROSS_X * s;
    out[VT3_TML_CROSS].cy = lh - CROSS_Y * s;
    out[VT3_TML_CROSS].r = CROSS_R * s;
    out[VT3_TML_CROSS].w = out[VT3_TML_CROSS].h = 0.0f;

    out[VT3_TML_B].kind = VT3_TML_CIRCLE;
    out[VT3_TML_B].ctl = VT3_TC_B;
    out[VT3_TML_B].cx = lw - (FACE_X + FACE_GAP) * s;
    out[VT3_TML_B].cy = lh - FACE_Y * s;
    out[VT3_TML_B].r = FACE_R * s;
    out[VT3_TML_B].w = out[VT3_TML_B].h = 0.0f;

    out[VT3_TML_A].kind = VT3_TML_CIRCLE;
    out[VT3_TML_A].ctl = VT3_TC_A;
    out[VT3_TML_A].cx = lw - FACE_X * s;
    out[VT3_TML_A].cy = lh - FACE_Y * s;
    out[VT3_TML_A].r = FACE_R * s;
    out[VT3_TML_A].w = out[VT3_TML_A].h = 0.0f;

    /* The pill the match draws as well, kept so a touch-only player can still reach the port's own
     * quick menu from inside a game menu; the top strip is clear of a centred list. */
    out[VT3_TML_MENU].kind = VT3_TML_PILL;
    out[VT3_TML_MENU].ctl = VT3_TC_MENU;
    out[VT3_TML_MENU].cx = lw * 0.5f;
    out[VT3_TML_MENU].cy = PILL_TOP * s + PILL_H * 0.5f * s;
    out[VT3_TML_MENU].r = 0.0f;
    out[VT3_TML_MENU].w = PILL_W * s;
    out[VT3_TML_MENU].h = PILL_H * s;
}

static float pick_radius(const vt3_tml_ctl *q)
{
    /* The same multipliers the match hit test uses, and the bigger one is the stick's. */
    return q->r * (q->ctl == VT3_TC_STICK ? VT3_TOUCH_PICK_STICK : VT3_TOUCH_PICK_BUTTON);
}

static float pill_pad(const vt3_tml_ctl *q, float s)
{
    return (q->ctl == VT3_TC_MENU ? VT3_TOUCH_PICK_MENU : VT3_TOUCH_PICK_PILL) * s;
}

int vt3_touch_menu_hit(const vt3_tml_ctl *ctls, int n, float x, float y, float s)
{
    if (!ctls) return -1;
    if (s <= 0.0f) s = 1.0f;

    int best = -1;
    float best_d = 0.0f;
    for (int i = 0; i < n; i++) {
        const vt3_tml_ctl *q = &ctls[i];
        float dx = x - q->cx, dy = y - q->cy;
        if (q->kind == VT3_TML_PILL) {
            float pad = pill_pad(q, s);
            if (x < q->cx - q->w * 0.5f - pad || x > q->cx + q->w * 0.5f + pad) continue;
            if (y < q->cy - q->h * 0.5f - pad || y > q->cy + q->h * 0.5f + pad) continue;
        } else {
            if (sqrtf(dx * dx + dy * dy) > pick_radius(q)) continue;
        }
        /* Inside: the nearest of the picks that claim the point wins, as in the match hit test. */
        float d = sqrtf(dx * dx + dy * dy);
        if (best < 0 || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    return best;
}
