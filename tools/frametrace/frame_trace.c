/*
 * Reads a capture taken with VT3_TRACE_FRAMES=1 ("frame: N draws" for every presented frame) and
 * prints the draws-per-frame range of each screen, so the menu-mode threshold
 * (VT3_TOUCH_MATCH_DRAWS, 96 by default) can be checked against the real game before trusting it.
 *
 *   adb logcat -v time > capture.log        # with VT3_TRACE_FRAMES=1 in vt3.env
 *   tools/frametrace/frame_trace capture.log
 *   adb logcat -v time | tools/frametrace/frame_trace
 *   tools/frametrace/frame_trace capture.log -t 96 -g 1000
 *
 * Every frame goes through vt3_menu_detect_update -- the same rule the overlay runs -- so the kinds
 * are what the runtime decided, not a second opinion. A screen is a run of one kind between gaps of
 * -g ms; a run shorter than the detector's own settling time is a transition and is folded into its
 * neighbour, and the first and last few frames of a screen (where a change is still settling) are
 * not counted against the level. What is reported per screen is therefore the screen itself.
 *
 * Exit status: 0 when the capture agrees with the level, 1 when it does not, 2 when there was
 * nothing to read.
 */
#include "vt3/vt3_touch_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES  2000000
#define EDGE_FRAMES      16      /* frames at each end of a screen that are still settling */
#define SHORT_SCREEN     48      /* below this a screen is all edge, and is reported whole */
#define MIN_SCREEN       16      /* shorter than this is a change between screens, not one */

typedef struct {
    int   draws;
    float t;          /* seconds from the first frame of the capture */
    int   have_t;
} frame_rec;

typedef struct {
    int   menu;
    long  start, len; /* [start, start + len) in the frame array */
} run_rec;

static frame_rec *frames;
static long nframes;

static int    level = VT3_MENU_DETECT_MATCH_DRAWS;
static double gap_ms = 1000.0;

static void fmt_time(char *buf, size_t n, float t)
{
    if (t < 0.0f) t = 0.0f;
    int m = (int)(t / 60.0f);
    snprintf(buf, n, "%d:%04.1f", m, (double)(t - (float)m * 60.0f));
}

static int parse_draws(const char *line, int *out)
{
    const char *p = strstr(line, "frame:");
    if (!p) return 0;
    p += 6;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    while (*end == ' ') end++;
    if (strncmp(end, "draws", 5) != 0) return 0;
    if (v < 0) v = 0;
    if (v > 1000000) v = 1000000;
    *out = (int)v;
    return 1;
}

/* logcat -v time / threadtime: "11-28 12:34:56.789 ..." */
static int parse_time(const char *line, double *out)
{
    int mon, day, hh, mm;
    double ss;
    if (sscanf(line, "%2d-%2d %2d:%2d:%lf", &mon, &day, &hh, &mm, &ss) != 5) return 0;
    if (mon < 1 || mon > 12 || day < 1 || day > 31) return 0;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0.0 || ss >= 61.0) return 0;
    *out = (double)(day * 24 + hh) * 3600.0 + (double)mm * 60.0 + ss;
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) level = atoi(argv[++i]);
        else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) gap_ms = atof(argv[++i]);
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: %s [capture.log] [-t draws] [-g gap-ms]\n", argv[0]);
            return 2;
        } else path = argv[i];
    }
    if (level < 1) level = 1;
    if (gap_ms < 0.0) gap_ms = 0.0;

    FILE *f = path ? fopen(path, "r") : stdin;
    if (!f) {
        fprintf(stderr, "frame_trace: cannot open %s\n", path);
        return 2;
    }

    frames = malloc(sizeof *frames * MAX_FRAMES);
    if (!frames) {
        fprintf(stderr, "frame_trace: out of memory\n");
        if (path) fclose(f);
        return 2;
    }

    char line[512];
    double t0 = 0.0;
    int have_base = 0;
    int truncated = 0;
    while (fgets(line, sizeof line, f)) {
        int draws;
        if (!parse_draws(line, &draws)) continue;
        if (nframes >= MAX_FRAMES) { truncated = 1; break; }
        double now = 0.0;
        int have_t = parse_time(line, &now);
        if (have_t && !have_base) { t0 = now; have_base = 1; }
        frames[nframes].draws = draws;
        frames[nframes].have_t = have_t;
        frames[nframes].t = have_t ? (float)(now - t0) : 0.0f;
        nframes++;
    }
    if (path) fclose(f);

    if (!nframes) {
        printf("nothing to read: no \"frame: N draws\" lines in the capture.\n"
               "Set VT3_TRACE_FRAMES=1 in vt3.env, run the game, and capture the log.\n");
        return 2;
    }

    /* Pass 1: the runtime's decision on every frame. */
    unsigned char *kind = calloc((size_t)nframes, 1);
    vt3_menu_detect_state det;
    memset(&det, 0, sizeof det);
    det.match_draws = (unsigned)level;
    for (long i = 0; i < nframes; i++) kind[i] = (unsigned char)vt3_menu_detect_update(&det, frames[i].draws);

    /* Runs of one kind, then every run too short to be a screen of its own becomes its neighbour's
     * kind: a change of screen is followed by a handful of frames that are still settling. */
    long nrun = 0;
    for (long i = 0; i < nframes; i++)
        if (i == 0 || kind[i] != kind[i - 1]) nrun++;
    run_rec *runs = calloc((size_t)nrun, sizeof *runs);
    if (!runs) { printf("frame_trace: out of memory\n"); return 2; }
    long r = 0;
    for (long i = 0; i < nframes; i++) {
        if (i == 0 || kind[i] != kind[i - 1]) {
            runs[r].menu = kind[i];
            runs[r].start = i;
            runs[r].len = 0;
            r++;
        }
        runs[r - 1].len++;
    }

    int settle = VT3_MENU_DETECT_MENU_FRAMES;
    for (long i = 0; i < nrun; i++) {
        if (runs[i].len >= settle) continue;
        /* The neighbour to take after: the previous run, or the next one for the first run. */
        long src = i > 0 ? i - 1 : (i + 1 < nrun ? i + 1 : -1);
        if (src < 0) continue;
        int k = runs[src].menu;
        for (long j = runs[i].start; j < runs[i].start + runs[i].len; j++) kind[j] = (unsigned char)k;
    }

    /* Pass 2: screens -- one kind, ended by a change of kind or by a gap in the capture. */
    typedef struct { int menu; long start, len; } screen_rec;
    screen_rec *screens = calloc((size_t)nframes, sizeof *screens);
    if (!screens) { printf("frame_trace: out of memory\n"); return 2; }
    long nscreens = 0;
    for (long i = 0; i < nframes; i++) {
        int new_screen = 0;
        if (!nscreens) new_screen = 1;
        else if (kind[i] != screens[nscreens - 1].menu) new_screen = 1;
        else if (frames[i].have_t && frames[i - 1].have_t &&
                 (double)(frames[i].t - frames[i - 1].t) * 1000.0 > gap_ms) new_screen = 1;
        if (new_screen) {
            screens[nscreens].menu = kind[i];
            screens[nscreens].start = i;
            screens[nscreens].len = 0;
            nscreens++;
        }
        screens[nscreens - 1].len++;
    }

    printf("frame trace%s%s: draws per frame, screen by screen (level %d)\n\n",
           path ? " " : " from stdin", path ? path : "", level);
    printf("  #  kind     start   secs    fps  frames  draws min /   mean /   max  (settling frames not counted)\n");

    int    menu_top = -1, match_low = -1;
    long   menu_frames = 0, match_frames = 0, menu_screens = 0, match_screens = 0;
    long   worst_quiet = 0, worst_rally = 0;
    long   near_level_matches = 0;
    long   shown = 0;
    int    flagged = 0;

    long skipped = 0;
    for (long i = 0; i < nscreens; i++) {
        long len = screens[i].len, s0 = screens[i].start;
        if (len < MIN_SCREEN) {
            /* A handful of frames between two screens: the change still settling, its draws belong
             * to neither screen. Counted, not judged. */
            skipped += len;
            continue;
        }
        long edge = len >= SHORT_SCREEN ? EDGE_FRAMES : 0;
        long lo = s0 + edge, hi = s0 + len - edge;

        int dmin = frames[lo].draws, dmax = dmin;
        double sum = 0.0;
        long quiet = 0, rally = 0, quiet_max = 0, rally_max = 0;
        for (long j = lo; j < hi; j++) {
            int d = frames[j].draws;
            if (d < dmin) dmin = d;
            if (d > dmax) dmax = d;
            sum += (double)d;
            if (d >= level) { rally++; quiet = 0; if (rally > rally_max) rally_max = rally; }
            else            { quiet++; rally = 0; if (quiet > quiet_max) quiet_max = quiet; }
        }
        long counted = hi - lo;

        char start[16] = "-", secs[16] = "-", fps[16] = "-";
        if (frames[s0].have_t && frames[s0 + len - 1].have_t) {
            float dt = frames[s0 + len - 1].t - frames[s0].t;
            fmt_time(start, sizeof start, frames[s0].t);
            snprintf(secs, sizeof secs, "%.1f", (double)dt);
            snprintf(fps, sizeof fps, "%.1f", dt > 0.001f ? (double)len / (double)dt : 0.0);
        }

        char note[96] = "";
        if (screens[i].menu) {
            menu_screens++;
            menu_frames += counted;
            if (dmax > menu_top) menu_top = dmax;
            if (rally_max > worst_rally) worst_rally = rally_max;
            if (dmax >= level) {
                snprintf(note, sizeof note, "   <-- %d draws is at or over the level", dmax);
                flagged = 1;
            }
        } else {
            match_screens++;
            match_frames += counted;
            if (match_low < 0 || dmin < match_low) match_low = dmin;
            if (quiet_max > worst_quiet) worst_quiet = quiet_max;
            /* A screen called a match that draws close to the level is the one to look at twice:
             * if it was really a menu, the level is set too low. */
            if ((long)dmin * 2 < (long)level * 3) near_level_matches++;
            if (quiet_max >= VT3_MENU_DETECT_MENU_FRAMES) {
                snprintf(note, sizeof note, "   <-- %ld quiet frames in a row would enter menu mode",
                         quiet_max);
                flagged = 1;
            }
        }

        printf("  %2ld  %-5s %6s %6s %6s %7ld   %5d / %6.1f / %5d%s\n", ++shown,
               screens[i].menu ? "menu" : "match", start, secs, fps, counted, dmin,
               sum / (double)counted, dmax, note);
    }

    printf("\n  %ld frames%s: %ld menu screen(s) with %ld frames, %ld match screen(s) with %ld frames\n",
           nframes, truncated ? " (capture truncated)" : "", menu_screens, menu_frames,
           match_screens, match_frames);
    if (skipped) printf("  %ld transition frames skipped (runs under %d frames)\n", skipped, MIN_SCREEN);
    if (menu_top >= 0) printf("  highest draw count on a menu screen:  %d\n", menu_top);
    if (match_low >= 0) printf("  lowest draw count on a match screen:  %d\n", match_low);
    printf("  longest run of rally frames inside a menu:  %ld (2 would leave menu mode)\n", worst_rally);
    printf("  longest run of quiet frames inside a match: %ld (8 would enter menu mode)\n", worst_quiet);
    if (near_level_matches)
        printf("  %ld match screen(s) drawing under 1.5x the level: check those really were matches\n",
               near_level_matches);

    if (menu_top >= 0 && match_low >= 0) {
        if (menu_top >= match_low) {
            printf("\n  the two overlap: a menu reached %d and a match went as low as %d, so no single\n"
                   "  level separates them.\n", menu_top, match_low);
            flagged = 1;
        } else {
            printf("\n  suggested level %d: between the highest menu (%d) and the lowest match (%d)\n",
                   menu_top + (match_low - menu_top + 1) / 2, menu_top, match_low);
        }
    } else if (menu_top >= 0) {
        printf("\n  no match frames in this capture: the menus read as menus (highest %d), but the\n"
               "  level is not tested against a rally. Capture a point too.\n", menu_top);
        flagged = 1;
    } else if (match_low >= 0) {
        printf("\n  no menu frames in this capture: everything reads as a match (lowest %d), but the\n"
               "  level is not tested against a menu. Capture the boot and title screens too.\n", match_low);
        flagged = 1;
    }

    if (flagged) {
        printf("\n  DISAGREES with level %d: set VT3_TOUCH_MATCH_DRAWS and capture again.\n", level);
        return 1;
    }
    printf("\n  agrees with level %d.\n", level);
    return 0;
}
