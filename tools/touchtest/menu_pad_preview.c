/*
 * Prints the touch overlay's menu-mode pad as ASCII for a frame size, so its placement can be
 * judged without a phone.
 *
 * It draws from vt3_touch_menu_layout and vt3_touch_menu_hit -- the same numbers the overlay draws
 * and taps -- so what is printed is what the screen gets, not an illustration of it.
 *
 *   cc -O1 -o /tmp/menupad tools/touchtest/menu_pad_preview.c runtime/src/ui/touch_menu_layout.c \
 *      -Iruntime/include -lm
 *   /tmp/menupad [width] [height] [size-scale] [columns]
 *
 * Defaults are 1920x1080 at size scale 1 and 120 columns. The legend separates what is drawn from
 * the extra area that still counts as a hit (the pick tolerance), which is what makes the pad
 * comfortably tappable without covering the game's list.
 */
#include "vt3/vt3_touch_menu_layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* One character per entry: the drawn shape, then the pick zone around it. */
static const char solid[VT3_TML_ENTRIES] = { '#', 'B', 'A', 'M' };
static const char faint[VT3_TML_ENTRIES] = { '+', 'b', 'a', 'm' };

static float point_to_pill(float px, float py, const vt3_tml_ctl *q)
{
    float dx = fabsf(px - q->cx) - q->w * 0.5f;
    float dy = fabsf(py - q->cy) - q->h * 0.5f;
    if (dx < 0.0f) dx = 0.0f;
    if (dy < 0.0f) dy = 0.0f;
    return sqrtf(dx * dx + dy * dy);
}

/* Is the point inside what the entry actually draws (not its pick zone)? */
static int in_shape(const vt3_tml_ctl *q, float x, float y)
{
    if (q->kind == VT3_TML_PILL) return point_to_pill(x, y, q) <= 0.0f;
    float dx = x - q->cx, dy = y - q->cy;
    return sqrtf(dx * dx + dy * dy) <= q->r;
}

static float drawn_gap(const vt3_tml_ctl *a, const vt3_tml_ctl *b)
{
    if (a->kind == VT3_TML_PILL || b->kind == VT3_TML_PILL) {
        const vt3_tml_ctl *pill = a->kind == VT3_TML_PILL ? a : b;
        const vt3_tml_ctl *circ = pill == a ? b : a;
        return point_to_pill(circ->cx, circ->cy, pill) - circ->r;
    }
    return hypotf(a->cx - b->cx, a->cy - b->cy) - (a->r + b->r);
}

static const char *ctl_name(int ctl)
{
    switch (ctl) {
    case VT3_TC_STICK: return "cross";
    case VT3_TC_A:     return "A";
    case VT3_TC_B:     return "B";
    case VT3_TC_MENU:  return "MENU";
    default:           return "?";
    }
}

int main(int argc, char **argv)
{
    float lw = argc > 1 ? (float)atof(argv[1]) : 1920.0f;
    float lh = argc > 2 ? (float)atof(argv[2]) : 1080.0f;
    float size = argc > 3 ? (float)atof(argv[3]) : 1.0f;
    int cols = argc > 4 ? atoi(argv[4]) : 120;

    if (lw < 32.0f || lh < 32.0f || size <= 0.0f || cols < 20) {
        fprintf(stderr, "usage: %s [width>32] [height>32] [size-scale] [columns>=20]\n", argv[0]);
        return 2;
    }

    float s = lh / 1080.0f * size;
    vt3_tml_ctl m[VT3_TML_ENTRIES];
    vt3_touch_menu_layout(lw, lh, s, m);

    /* Terminal cells are about twice as tall as they are wide, so halve the row count to keep the
     * picture in the frame's true proportion. */
    int rows = (int)((float)cols * lh / (2.0f * lw) + 0.5f);
    if (rows < 8) rows = 8;

    printf("menu-mode touch pad   %.0fx%.0f   size scale %.2f   scale %.3f   (%d x %d cells)\n\n",
           lw, lh, size, s, cols, rows);
    for (int r = 0; r < rows; r++) {
        putchar(' ');
        for (int c = 0; c < cols; c++) {
            float x = (float)(c + 0.5f) * lw / (float)cols;
            float y = (float)(r + 0.5f) * lh / (float)rows;
            int hit = vt3_touch_menu_hit(m, VT3_TML_ENTRIES, x, y, s);
            if (hit >= 0 && in_shape(&m[hit], x, y)) putchar(solid[hit]);
            else if (hit >= 0) putchar(faint[hit]);
            else putchar(c % 4 == 0 && r % 2 == 0 ? '.' : ' ');
        }
        putchar('\n');
    }

    printf("\n  # cross (d-pad)   B / A buttons   M MENU pill\n");
    printf("  + a b m = pick zone beyond the drawn shape   . empty screen\n\n");

    printf("  %-6s %-16s %-14s %-18s\n", "ctl", "centre", "size", "centre in frame");
    for (int i = 0; i < VT3_TML_ENTRIES; i++) {
        char centre[32], dims[32], frac[32];
        snprintf(centre, sizeof centre, "%.0f, %.0f", m[i].cx, m[i].cy);
        if (m[i].kind == VT3_TML_PILL)
            snprintf(dims, sizeof dims, "%.0f x %.0f", m[i].w, m[i].h);
        else
            snprintf(dims, sizeof dims, "r %.0f", m[i].r);
        snprintf(frac, sizeof frac, "%.1f%%, %.1f%%", 100.0f * m[i].cx / lw, 100.0f * m[i].cy / lh);
        printf("  %-6s %-16s %-14s %-18s\n", ctl_name(m[i].ctl), centre, dims, frac);
    }

    /* The gaps are the numbers to eyeball: positive means the shapes are clear of each other. */
    printf("\n  drawn gaps (px):");
    for (int i = 0; i < VT3_TML_ENTRIES; i++)
        for (int j = i + 1; j < VT3_TML_ENTRIES; j++)
            printf("  %s-%s %.0f", ctl_name(m[i].ctl), ctl_name(m[j].ctl), drawn_gap(&m[i], &m[j]));
    printf("\n");
    return 0;
}
