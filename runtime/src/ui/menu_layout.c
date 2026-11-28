/*
 * The quick menu's geometry. See vt3/vt3_menu_layout.h for why it is a module of its own.
 */
#include "vt3/vt3_menu_layout.h"

/* The plate's fixed parts, in logical pixels: the PAUSED heading above the rows, and the hint line
 * under them. */
#define PANEL_HEAD 66.0f
#define PANEL_FOOT 34.0f

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void vt3_menu_panel(float lw, float lh, float *x, float *y, float *w, float *h)
{
    float pw = clampf(lw * 0.56f, 320.0f, 560.0f);
    float ph = PANEL_HEAD + (float)VT3_MENU_ROWS * (VT3_MENU_ROW_H + VT3_MENU_ROW_GAP) + PANEL_FOOT;
    if (w) *w = pw;
    if (h) *h = ph;
    if (x) *x = (lw - pw) * 0.5f;
    if (y) *y = (lh - ph) * 0.5f;
}

vt3_menu_row vt3_menu_row_rect(float lw, float lh, int row)
{
    vt3_menu_row r = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (row < 0 || row >= VT3_MENU_ROWS) return r;
    float x, y, w, h;
    vt3_menu_panel(lw, lh, &x, &y, &w, &h);
    /* The bar is inset 16 px from each side of the plate; the arrows live in that margin. */
    r.x = x + 16.0f;
    r.y = y + PANEL_HEAD + (float)row * (VT3_MENU_ROW_H + VT3_MENU_ROW_GAP);
    r.w = w - 32.0f;
    r.h = VT3_MENU_ROW_H;
    return r;
}

int vt3_menu_row_at(float lx, float ly, float lw, float lh)
{
    float x, y, w, h;
    vt3_menu_panel(lw, lh, &x, &y, &w, &h);
    if (lx < x || lx > x + w) return -1;          /* beside the plate: not a row */
    float half = VT3_MENU_ROW_GAP * 0.5f;
    float top = y + PANEL_HEAD;
    for (int i = 0; i < VT3_MENU_ROWS; i++) {
        float ry = top + (float)i * (VT3_MENU_ROW_H + VT3_MENU_ROW_GAP);
        if (ly >= ry - half && ly < ry + VT3_MENU_ROW_H + half) return i;
    }
    return -1;                                    /* the heading above, or the hints below */
}
