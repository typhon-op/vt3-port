/*
 * Host test for the touch overlay's menu mode (vt3_touch_menu_layout): the compact cross, A/B and
 * MENU pill drawn while the game is showing one of its own menus.
 *
 * A device is the only place the drawing can be looked at, so what is checked here is the part that
 * can be decided without one: every frame size the port runs at leaves the controls inside the
 * frame, clear of each other and of each other's pick zones, and every control that is drawn can
 * actually be hit -- at its centre, inside its shape, and on each arm of the cross, which is what
 * turns a tap into a single menu step.
 *
 *   cc -O1 -Wall -Wextra -o /tmp/touchmenutest tools/touchtest/TouchMenuHostTest.c \
 *      runtime/src/ui/touch_menu_layout.c -Iruntime/include -lm
 */
#include "vt3/vt3_touch_menu_layout.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;
static float frame_w, frame_h;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s (frame %gx%g)\n", what, frame_w, frame_h);
    }
}

/* Distance from a point to the nearest edge of a pill's drawn rect (0 when inside). */
static float point_to_pill(float px, float py, const vt3_tml_ctl *q)
{
    float dx = fabsf(px - q->cx) - q->w * 0.5f;
    float dy = fabsf(py - q->cy) - q->h * 0.5f;
    if (dx < 0.0f) dx = 0.0f;
    if (dy < 0.0f) dy = 0.0f;
    return sqrtf(dx * dx + dy * dy);
}

static float pick_radius(const vt3_tml_ctl *q)
{
    return q->r * (q->ctl == VT3_TC_STICK ? VT3_TOUCH_PICK_STICK : VT3_TOUCH_PICK_BUTTON);
}

/* Gap between what two entries draw: negative means they overlap. */
static float drawn_gap(const vt3_tml_ctl *a, const vt3_tml_ctl *b)
{
    if (a->kind == VT3_TML_PILL || b->kind == VT3_TML_PILL) {
        const vt3_tml_ctl *pill = a->kind == VT3_TML_PILL ? a : b;
        const vt3_tml_ctl *circ = pill == a ? b : a;
        return point_to_pill(circ->cx, circ->cy, pill) - circ->r;
    }
    return hypotf(a->cx - b->cx, a->cy - b->cy) - (a->r + b->r);
}

/* Gap between what two entries claim for a touch: negative means a tap could be ambiguous. */
static float pick_gap(const vt3_tml_ctl *a, const vt3_tml_ctl *b, float s)
{
    if (a->kind == VT3_TML_PILL || b->kind == VT3_TML_PILL) {
        const vt3_tml_ctl *pill = a->kind == VT3_TML_PILL ? a : b;
        const vt3_tml_ctl *circ = pill == a ? b : a;
        float pad = (pill->ctl == VT3_TC_MENU ? VT3_TOUCH_PICK_MENU : VT3_TOUCH_PICK_PILL) * s;
        return point_to_pill(circ->cx, circ->cy, pill) - pad - pick_radius(circ);
    }
    return hypotf(a->cx - b->cx, a->cy - b->cy) - (pick_radius(a) + pick_radius(b));
}

static int find(const vt3_tml_ctl *m, int ctl)
{
    for (int i = 0; i < VT3_TML_ENTRIES; i++)
        if (m[i].ctl == ctl) return i;
    return -1;
}

static void test_frame(float lw, float lh, float s)
{
    frame_w = lw;
    frame_h = lh;

    vt3_tml_ctl m[VT3_TML_ENTRIES];
    memset(m, 0x7f, sizeof m);            /* garbage in, checked out: the fill must cover it */
    vt3_touch_menu_layout(lw, lh, s, m);

    /* The set: one entry per control, and exactly the controls menu mode is meant to show. */
    int seen[VT3_TC_COUNT];
    memset(seen, 0, sizeof seen);
    int dup = 0, bad = 0;
    for (int i = 0; i < VT3_TML_ENTRIES; i++) {
        int c = m[i].ctl;
        if (c < 0 || c >= VT3_TC_COUNT) { bad = 1; continue; }
        if (seen[c]++) dup = 1;
    }
    check(!bad, "every entry names a real control");
    check(!dup, "no control appears twice");
    check(seen[VT3_TC_STICK] == 1, "the cross drives the stick");
    check(seen[VT3_TC_A] == 1 && seen[VT3_TC_B] == 1, "A and B are both present");
    check(seen[VT3_TC_MENU] == 1, "the MENU pill is present");
    for (int c = 0; c < VT3_TC_COUNT; c++) {
        if (c == VT3_TC_STICK || c == VT3_TC_A || c == VT3_TC_B || c == VT3_TC_MENU) continue;
        check(seen[c] == 0, "nothing else is drawn on a game menu");
    }

    /* Sizes and containment: a control half off the screen is a control half untappable. */
    for (int i = 0; i < VT3_TML_ENTRIES; i++) {
        const vt3_tml_ctl *q = &m[i];
        if (q->kind == VT3_TML_CIRCLE) {
            check(q->r > 0.0f, "a circle has a radius");
            check(q->cx - q->r >= 0.0f && q->cx + q->r <= lw, "a circle fits across the frame");
            check(q->cy - q->r >= 0.0f && q->cy + q->r <= lh, "a circle fits down the frame");
        } else {
            check(q->w > 0.0f && q->h > 0.0f, "the pill has a size");
            check(q->cx - q->w * 0.5f >= 0.0f && q->cx + q->w * 0.5f <= lw, "the pill fits across");
            check(q->cy - q->h * 0.5f >= 0.0f && q->cy + q->h * 0.5f <= lh, "the pill fits down");
        }
    }

    /* No drawn overlap. A and B's pick zones are allowed to touch -- the nearest wins, as in a
     * match -- but the cross's zone must not reach a face button, or a direction tap would read as
     * a confirm, and the pill must reach nothing at all. */
    for (int i = 0; i < VT3_TML_ENTRIES; i++) {
        for (int j = i + 1; j < VT3_TML_ENTRIES; j++) {
            check(drawn_gap(&m[i], &m[j]) >= 0.0f, "the controls do not overlap");
            int narrow = m[i].ctl == VT3_TC_STICK || m[j].ctl == VT3_TC_STICK
                      || m[i].ctl == VT3_TC_MENU || m[j].ctl == VT3_TC_MENU;
            if (narrow) {
                check(pick_gap(&m[i], &m[j], s) > 0.0f,
                      "the cross and the pill claim nothing else's area");
                /* And each shape's own centre is settled by distance, never by a tie. */
                check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, m[i].cx, m[i].cy, s) == i, "still its own");
                check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, m[j].cx, m[j].cy, s) == j, "still its own");
            }
        }
    }

    /* Between A and B the nearer one takes the touch, which is the rule that lets their pick zones
     * touch at all. */
    int ai = find(m, VT3_TC_A), bi = find(m, VT3_TC_B);
    if (ai >= 0 && bi >= 0) {
        float mx = (m[ai].cx + m[bi].cx) * 0.5f;
        check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, mx - 1.0f, m[ai].cy, s) == bi,
              "left of the midpoint is B");
        check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, mx + 1.0f, m[ai].cy, s) == ai,
              "right of the midpoint is A");
    }

    /* Every drawn control can be hit: at its centre, and well inside its own shape. */
    for (int i = 0; i < VT3_TML_ENTRIES; i++) {
        const vt3_tml_ctl *q = &m[i];
        check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, q->cx, q->cy, s) == i,
              "a control is hit at its centre");
        if (q->kind == VT3_TML_CIRCLE) {
            float k = q->r * 0.7f;
            check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, q->cx - k, q->cy, s) == i,
                  "hit to the left inside a button");
            check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, q->cx, q->cy + k, s) == i,
                  "hit below centre inside a button");
        } else {
            check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, q->cx, q->cy + q->h * 0.45f, s) == i,
                  "hit inside the bottom of the pill");
            check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, q->cx + q->w * 0.45f, q->cy, s) == i,
                  "hit inside the right of the pill");
        }
    }

    /* The four arms: each is a tap on the cross, which the d-pad path turns into one step. */
    int si = find(m, VT3_TC_STICK);
    if (si >= 0) {
        const vt3_tml_ctl *cross = &m[si];
        float k = cross->r * 0.6f;
        check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, cross->cx, cross->cy - k, s) == si, "the up arm hits the cross");
        check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, cross->cx, cross->cy + k, s) == si, "the down arm hits the cross");
        check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, cross->cx - k, cross->cy, s) == si, "the left arm hits the cross");
        check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, cross->cx + k, cross->cy, s) == si, "the right arm hits the cross");
        /* And outside the drawn ring, within the pick tolerance, is still the stick. */
        check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, cross->cx, cross->cy - cross->r * 1.5f, s) == si,
              "a near miss outside the ring still reaches the cross");
    }

    /* The empty middle of the screen is not a control: a tap there must do nothing. */
    check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, lw * 0.5f, lh * 0.5f, s) == -1,
          "the middle of the screen hits nothing");
    /* Nor is a corner nothing is drawn in (the top right, above A). */
    check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, lw - 2.0f, 2.0f, s) == -1,
          "the empty top corner hits nothing");
}

int main(void)
{
    /* The landscape sizes the port runs at: 4:3, 16:10, 16:9, 19.5:9 and 21:9, small to large. */
    static const float frames[][2] = {
        {  480,  320 }, {  640,  480 }, {  800,  600 }, { 1024,  768 },
        { 1280,  720 }, { 1366,  768 }, { 1920, 1080 }, { 2340, 1080 },
        { 2560, 1080 }, { 2560, 1440 },
    };
    for (unsigned i = 0; i < sizeof frames / sizeof frames[0]; i++)
        test_frame(frames[i][0], frames[i][1], frames[i][1] / 1080.0f);

    /* The user's size setting scales the pad with the frame, so the same checks hold around the
     * extremes the settings screen allows. */
    static const float scales[] = { 0.8f, 1.0f, 1.25f };
    for (unsigned i = 0; i < sizeof scales / sizeof scales[0]; i++)
        test_frame(1280.0f, 720.0f, 720.0f / 1080.0f * scales[i]);

    /* A degenerate frame must not produce nonsense geometry (the overlay never draws one, but the
     * layout is called before the frame size is known to be sane). */
    vt3_tml_ctl m[VT3_TML_ENTRIES];
    vt3_touch_menu_layout(0.0f, 0.0f, 0.0f, m);
    check(m[VT3_TML_CROSS].r > 0.0f, "a zero scale still lays out a real cross");
    check(vt3_touch_menu_hit(m, VT3_TML_ENTRIES, m[VT3_TML_A].cx, m[VT3_TML_A].cy, 0.0f) >= 0,
          "and its buttons are still hittable");

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
