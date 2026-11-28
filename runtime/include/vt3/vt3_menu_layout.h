/*
 * Where the port's quick menu puts its panel and its rows, in the renderer's logical pixels.
 *
 * One home for the numbers: the panel is drawn from these, and a tap is hit-tested against these,
 * so a row can never be drawn somewhere it cannot be tapped (or tapped somewhere it is not drawn).
 * Pure arithmetic, no renderer and no state, so it is checked on the host (tools/touchtest).
 */
#ifndef VT3_MENU_LAYOUT_H
#define VT3_MENU_LAYOUT_H

/* The quick menu's rows, in the order they are drawn and dispatched. */
#define VT3_MENU_ROWS 6
#define VT3_MENU_ROW_H 40.0f
#define VT3_MENU_ROW_GAP 6.0f

typedef struct vt3_menu_row {
    float x, y, w, h;
} vt3_menu_row;

/* The panel for a logical frame: centred, 56% of the width clamped to 320..560 px. */
void vt3_menu_panel(float lw, float lh, float *x, float *y, float *w, float *h);

/* Row `row`'s drawn bar, or an empty rect for a row out of range. */
vt3_menu_row vt3_menu_row_rect(float lw, float lh, int row);

/* The row a point picks, or -1. The whole panel width counts, and each row's band includes half
 * the gap on either side, so a thumb that lands between two rows still picks one of them.
 * lw and lh are the renderer's logical frame and must be positive (a caller with no frame yet has
 * nothing on screen to hit-test). */
int vt3_menu_row_at(float lx, float ly, float lw, float lh);

#endif /* VT3_MENU_LAYOUT_H */
