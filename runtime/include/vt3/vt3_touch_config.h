/*
 * The on-screen controls' saved configuration: layouts, stick profiles, the swipe pad and the lock.
 *
 * One small text file in the game's data root, "touch.cfg", key=value per line, versioned. The Java
 * screens write it (TouchConfig.java) and this module reads it in the runtime, so the two never
 * disagree about what a setting means: every key here is one this module acts on.
 *
 * A bad or half-written file never takes the controls down: loading fails, the caller keeps the
 * defaults it already had, and TouchConfig.java keeps the previous good copy beside it.
 */
#ifndef VT3_TOUCH_CONFIG_H
#define VT3_TOUCH_CONFIG_H

#include "vt3/vt3_touch_core.h"

/* Controls in the order the renderer and the editor use. SWIPE is the one-button gesture pad. */
enum {
    VT3_TC_STICK = 0,
    VT3_TC_A, VT3_TC_B, VT3_TC_X, VT3_TC_Y,
    VT3_TC_LB, VT3_TC_RB,
    VT3_TC_SELECT, VT3_TC_START,
    VT3_TC_SWIPE,
    VT3_TC_MENU,
    VT3_TC_COUNT
};

enum { VT3_LAYOUT_STANDARD = 0, VT3_LAYOUT_COMPACT, VT3_LAYOUT_SWIPE, VT3_LAYOUT_CUSTOM1, VT3_LAYOUT_CUSTOM2,
       VT3_LAYOUT_COUNT };

enum { VT3_PROFILE_DEFAULT = 0, VT3_PROFILE_CLASSIC, VT3_PROFILE_FLOATING, VT3_PROFILE_DYNAMIC,
       VT3_PROFILE_CUSTOM1, VT3_PROFILE_CUSTOM2, VT3_PROFILE_COUNT };

#define VT3_TOUCH_CFG_VERSION 1
#define VT3_TOUCH_CFG_MAX 16384

/* Per-control placement inside one layout. x and y are the control's centre as a fraction of the
 * logical width and height; a negative value means "the built-in position", which is how the
 * Standard layout keeps the exact geometry 0.3.0 shipped. size scales the drawn control, opacity
 * its alpha, visible=0 hides it, removed=0 means it is gone from the layout entirely. */
typedef struct {
    float x, y;
    float size;
    float opacity;
    int   visible;
    int   removed;
} vt3_touch_ctl_cfg;

typedef struct {
    int   version;
    int   layout;
    int   profile;
    int   locked;

    int   swipe_enabled;
    int   swipe_labels;
    int   swipe_charge;
    float swipe_min_dist;          /* fraction of the pad's half extent (0.02..0.5) */
    int   swipe_map[4];            /* up, down, left, right -> VT3_TC_A..VT3_TC_Y */

    vt3_touch_ctl_cfg ctl[VT3_LAYOUT_COUNT][VT3_TC_COUNT];
    vt3_stick_cfg     prof[VT3_PROFILE_COUNT];
} vt3_touch_cfg;

/* Fills cfg with the shipped defaults: Standard layout, Default profile, unlocked, swipe enabled
 * but laid out where the Swipe preset puts it (hidden until that preset or a custom layout asks
 * for it). */
void vt3_touch_cfg_defaults(vt3_touch_cfg *cfg);

/* The built-in geometry for one layout, without any user override. Used by the renderer and by the
 * editor's reset. */
void vt3_touch_cfg_preset(vt3_touch_cfg *cfg, int layout);

/* 0 on success; -1 when the file is missing; -2 when it is malformed (cfg is then untouched, so
 * the caller's defaults or its previous good copy stay in force). Unknown keys and out-of-range
 * values are ignored and clamped rather than failing the whole file, so a newer file from a later
 * version still loads. */
int vt3_touch_cfg_load(vt3_touch_cfg *cfg, const char *path);

/* Writes through a temporary file and renames it into place, so a crash mid-write leaves the old
 * file intact. Returns 0 on success. */
int vt3_touch_cfg_save(const vt3_touch_cfg *cfg, const char *path);

/* The profile the selected index refers to. */
const vt3_stick_cfg *vt3_touch_cfg_profile(const vt3_touch_cfg *cfg);

/* The control's placement in the selected layout. */
const vt3_touch_ctl_cfg *vt3_touch_cfg_ctl(const vt3_touch_cfg *cfg, int ctl);

#endif /* VT3_TOUCH_CONFIG_H */
