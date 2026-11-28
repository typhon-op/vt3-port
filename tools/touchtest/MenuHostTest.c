/*
 * Host checks for the quick menu's geometry: the panel and row rectangles the menu draws, and the
 * row a tap picks. The interesting property is the one the port depends on — the bar drawn for row
 * i is inside the band that hit-tests to row i — so the test asserts the two against each other
 * rather than against a copy of the formula.
 *
 * Build: cc -O1 -o /tmp/menutest tools/touchtest/MenuHostTest.c runtime/src/ui/menu_layout.c \
 *            -Iruntime/include -lm
 */
#include "vt3/vt3_menu_layout.h"

#include <math.h>
#include <stdio.h>

static int g_checks, g_fails;

static void check(int ok, const char *what)
{
    g_checks++;
    if (!ok) {
        g_fails++;
        printf("FAIL %s\n", what);
    }
}

/* Every row's drawn bar must hit-test to that row, at every size the panel is used at. */
static void rows_hit_themselves(float lw, float lh)
{
    char what[160];
    for (int i = 0; i < VT3_MENU_ROWS; i++) {
        vt3_menu_row r = vt3_menu_row_rect(lw, lh, i);
        float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        snprintf(what, sizeof what, "%gx%g: centre of row %d hits row %d", lw, lh, i, i);
        check(vt3_menu_row_at(cx, cy, lw, lh) == i, what);

        /* The plate's margin beside the bar counts too: a thumb rarely lands dead centre. */
        snprintf(what, sizeof what, "%gx%g: plate margin beside row %d hits row %d", lw, lh, i, i);
        check(vt3_menu_row_at(r.x - 10.0f, cy, lw, lh) == i, what);

        /* Top and bottom of the bar itself, just inside. */
        snprintf(what, sizeof what, "%gx%g: top edge of row %d", lw, lh, i);
        check(vt3_menu_row_at(cx, r.y + 1.0f, lw, lh) == i, what);
        snprintf(what, sizeof what, "%gx%g: bottom edge of row %d", lw, lh, i);
        check(vt3_menu_row_at(cx, r.y + r.h - 1.0f, lw, lh) == i, what);
    }
}

static void one_size(float lw, float lh)
{
    char what[200];
    float x, y, w, h;
    vt3_menu_panel(lw, lh, &x, &y, &w, &h);

    snprintf(what, sizeof what, "%gx%g: panel is centred", lw, lh);
    check(fabsf((x + w * 0.5f) - lw * 0.5f) < 0.01f, what);
    check(fabsf((y + h * 0.5f) - lh * 0.5f) < 0.01f, what);
    snprintf(what, sizeof what, "%gx%g: panel width is clamped to 320..560", lw, lh);
    check(w >= 320.0f - 0.01f && w <= 560.0f + 0.01f, what);

    /* Rows march down the plate with an even gap, inside it, and never overlap. */
    for (int i = 0; i < VT3_MENU_ROWS; i++) {
        vt3_menu_row a = vt3_menu_row_rect(lw, lh, i);
        snprintf(what, sizeof what, "%gx%g: row %d is inside the plate", lw, lh, i);
        check(a.x >= x - 0.01f && a.x + a.w <= x + w + 0.01f, what);
        check(a.y >= y + 0.01f && a.y + a.h <= y + h + 0.01f, what);
        snprintf(what, sizeof what, "%gx%g: row %d has the row height", lw, lh, i);
        check(fabsf(a.h - VT3_MENU_ROW_H) < 0.01f, what);
        if (i == 0) continue;
        vt3_menu_row b = vt3_menu_row_rect(lw, lh, i - 1);
        snprintf(what, sizeof what, "%gx%g: rows %d and %d are one gap apart", lw, lh, i - 1, i);
        check(fabsf((a.y - (b.y + b.h)) - VT3_MENU_ROW_GAP) < 0.01f, what);
    }

    rows_hit_themselves(lw, lh);

    /* Above the rows (the PAUSED heading) and below them (the hint line) nothing is tappable. */
    snprintf(what, sizeof what, "%gx%g: the heading is not a row", lw, lh);
    check(vt3_menu_row_at(x + w * 0.5f, y + 20.0f, lw, lh) == -1, what);
    snprintf(what, sizeof what, "%gx%g: the hints are not a row", lw, lh);
    check(vt3_menu_row_at(x + w * 0.5f, y + h - 10.0f, lw, lh) == -1, what);

    /* Beside the plate: outside. */
    snprintf(what, sizeof what, "%gx%g: left of the plate is not a row", lw, lh);
    check(vt3_menu_row_at(x - 4.0f, y + h * 0.5f, lw, lh) == -1, what);
    snprintf(what, sizeof what, "%gx%g: right of the plate is not a row", lw, lh);
    check(vt3_menu_row_at(x + w + 4.0f, y + h * 0.5f, lw, lh) == -1, what);

    /* The middle of the gap between two rows belongs to the lower one, so every point in the row
     * area picks something and the boundary is not ambiguous. */
    vt3_menu_row r0 = vt3_menu_row_rect(lw, lh, 0);
    vt3_menu_row r1 = vt3_menu_row_rect(lw, lh, 1);
    float mid = r0.y + r0.h + VT3_MENU_ROW_GAP * 0.5f;
    snprintf(what, sizeof what, "%gx%g: the gap's middle goes to the lower row", lw, lh);
    check(vt3_menu_row_at(r0.x + r0.w * 0.5f, mid, lw, lh) == 1, what);
    check(r1.y - mid > 0.0f, "the gap midpoint is above row 1");

    /* Out-of-range rows are empty rects, not garbage. */
    vt3_menu_row none = vt3_menu_row_rect(lw, lh, VT3_MENU_ROWS);
    snprintf(what, sizeof what, "%gx%g: row %d is an empty rect", lw, lh, VT3_MENU_ROWS);
    check(none.w == 0.0f && none.h == 0.0f, what);
    check(vt3_menu_row_rect(lw, lh, -1).w == 0.0f, "row -1 is an empty rect");
}

int main(void)
{
    /* The sizes the port actually renders at: 4:3, widescreen, and the smallest logical frame. */
    one_size(640.0f, 480.0f);
    one_size(1280.0f, 720.0f);
    one_size(320.0f, 240.0f);
    one_size(1920.0f, 1080.0f);

    /* Degenerate input must not crash or index out of bounds. A real caller never passes a frame
     * without a size (the menu's tap path rejects one), so the contract is only "no row, or a row
     * that exists", never a row index off the end. */
    int zero = vt3_menu_row_at(0.0f, 0.0f, 0.0f, 0.0f);
    check(zero == -1 || (zero >= 0 && zero < VT3_MENU_ROWS), "a zero-sized frame yields no row or a real one");

    printf("%d checks, %d failure(s)\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
