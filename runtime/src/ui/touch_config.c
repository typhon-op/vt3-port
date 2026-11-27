/*
 * touch.cfg: reading, validating and writing the on-screen controls' settings.
 *
 * The format is deliberately boring, one "key=value" per line, so a file written by TouchConfig.java
 * can be read here with no parser library and no shared code beyond the key names, and a person can
 * read it with a file manager.
 */
#include "vt3/vt3_touch_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const CTL_NAME[VT3_TC_COUNT] = {
    "stick", "a", "b", "x", "y", "lb", "rb", "select", "start", "swipe"
};
static const char *const LAYOUT_NAME[VT3_LAYOUT_COUNT] = {
    "standard", "compact", "swipe", "custom1", "custom2"
};
static const char *const PROFILE_NAME[VT3_PROFILE_COUNT] = {
    "default", "classic", "floating", "dynamic", "custom1", "custom2"
};
static const char *const DIR_NAME[4] = { "up", "down", "left", "right" };

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void set_defaults_for(vt3_touch_cfg *c)
{
    for (int l = 0; l < VT3_LAYOUT_COUNT; l++) {
        for (int k = 0; k < VT3_TC_COUNT; k++) {
            c->ctl[l][k].x = -1.0f;      /* -1: the built-in position, exactly as 0.3.0 drew it */
            c->ctl[l][k].y = -1.0f;
            c->ctl[l][k].size = 1.0f;
            c->ctl[l][k].opacity = 1.0f;
            c->ctl[l][k].visible = 1;
            c->ctl[l][k].removed = 0;
        }
    }
}

static void profile_defaults(vt3_touch_cfg *c)
{
    for (int i = 0; i < VT3_PROFILE_COUNT; i++) {
        vt3_stick_cfg *p = &c->prof[i];
        p->deadzone = 0.14f;
        p->sensitivity = 1.0f;
        p->range = 1.0f;
        p->progressive = 0;
        p->floating = 0;
        p->smooth_return = 0;
        p->invert_x = p->invert_y = 0;
        p->sens_x = p->sens_y = 1.0f;
    }
    /* Classic: the same fixed stick with a slightly smaller dead zone, which is all the name
     * promises; Floating: the centre follows the thumb; Dynamic: progressive response. */
    c->prof[VT3_PROFILE_CLASSIC].deadzone = 0.10f;
    c->prof[VT3_PROFILE_FLOATING].floating = 1;
    c->prof[VT3_PROFILE_DYNAMIC].progressive = 1;
}

void vt3_touch_cfg_defaults(vt3_touch_cfg *c)
{
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->version = VT3_TOUCH_CFG_VERSION;
    c->layout = VT3_LAYOUT_STANDARD;
    c->profile = VT3_PROFILE_DEFAULT;
    c->locked = 1;                       /* layout edits only inside the editor */
    c->swipe_enabled = 1;
    c->swipe_labels = 1;
    c->swipe_charge = 0;
    c->swipe_min_dist = 0.18f;
    c->swipe_map[0] = VT3_TC_A;          /* up */
    c->swipe_map[1] = VT3_TC_B;          /* down */
    c->swipe_map[2] = VT3_TC_X;          /* left */
    c->swipe_map[3] = VT3_TC_Y;          /* right */
    set_defaults_for(c);
    profile_defaults(c);
    vt3_touch_cfg_preset(c, VT3_LAYOUT_STANDARD);
}

void vt3_touch_cfg_preset(vt3_touch_cfg *c, int layout)
{
    if (!c || layout < 0 || layout >= VT3_LAYOUT_COUNT) return;
    for (int k = 0; k < VT3_TC_COUNT; k++) {
        c->ctl[layout][k].x = -1.0f;
        c->ctl[layout][k].y = -1.0f;
        c->ctl[layout][k].size = 1.0f;
        c->ctl[layout][k].opacity = 1.0f;
        c->ctl[layout][k].visible = 1;
        c->ctl[layout][k].removed = 0;
    }
    /* The swipe pad is optional and absent from the two layouts that keep the original buttons. */
    c->ctl[layout][VT3_TC_SWIPE].removed = 1;

    if (layout == VT3_LAYOUT_COMPACT) {
        c->ctl[layout][VT3_TC_STICK].x = 0.155f;
        c->ctl[layout][VT3_TC_STICK].y = 0.800f;
        c->ctl[layout][VT3_TC_STICK].size = 0.85f;
        static const float ax[4] = { 0.855f, 0.945f, 0.765f, 0.855f };   /* A, B, X, Y */
        static const float ay[4] = { 0.875f, 0.755f, 0.755f, 0.635f };
        for (int i = 0; i < 4; i++) {
            c->ctl[layout][VT3_TC_A + i].x = ax[i];
            c->ctl[layout][VT3_TC_A + i].y = ay[i];
            c->ctl[layout][VT3_TC_A + i].size = 0.85f;
        }
        c->ctl[layout][VT3_TC_LB].x = 0.135f; c->ctl[layout][VT3_TC_LB].y = 0.055f;
        c->ctl[layout][VT3_TC_RB].x = 0.865f; c->ctl[layout][VT3_TC_RB].y = 0.055f;
        c->ctl[layout][VT3_TC_LB].size = c->ctl[layout][VT3_TC_RB].size = 0.85f;
        c->ctl[layout][VT3_TC_SELECT].x = 0.420f; c->ctl[layout][VT3_TC_SELECT].y = 0.945f;
        c->ctl[layout][VT3_TC_START].x = 0.580f;  c->ctl[layout][VT3_TC_START].y = 0.945f;
        c->ctl[layout][VT3_TC_SELECT].size = c->ctl[layout][VT3_TC_START].size = 0.90f;
    } else if (layout == VT3_LAYOUT_SWIPE) {
        c->ctl[layout][VT3_TC_STICK].x = 0.170f;
        c->ctl[layout][VT3_TC_STICK].y = 0.820f;
        c->ctl[layout][VT3_TC_STICK].size = 0.90f;
        for (int i = 0; i < 4; i++) c->ctl[layout][VT3_TC_A + i].removed = 1;   /* the pad replaces them */
        c->ctl[layout][VT3_TC_SWIPE].removed = 0;
        c->ctl[layout][VT3_TC_SWIPE].x = 0.720f;
        c->ctl[layout][VT3_TC_SWIPE].y = 0.740f;
        c->ctl[layout][VT3_TC_SWIPE].size = 1.50f;
    }
}

const vt3_stick_cfg *vt3_touch_cfg_profile(const vt3_touch_cfg *c)
{
    if (!c) return NULL;
    int i = c->profile;
    if (i < 0 || i >= VT3_PROFILE_COUNT) i = VT3_PROFILE_DEFAULT;
    return &c->prof[i];
}

const vt3_touch_ctl_cfg *vt3_touch_cfg_ctl(const vt3_touch_cfg *c, int ctl)
{
    if (!c || ctl < 0 || ctl >= VT3_TC_COUNT) return NULL;
    int l = c->layout;
    if (l < 0 || l >= VT3_LAYOUT_COUNT) l = VT3_LAYOUT_STANDARD;
    return &c->ctl[l][ctl];
}

/* ---- reading --------------------------------------------------------------- */

static int name_index(const char *const *names, int n, const char *s, size_t len)
{
    for (int i = 0; i < n; i++)
        if (strlen(names[i]) == len && memcmp(names[i], s, len) == 0) return i;
    return -1;
}

static int parse_int(const char *v, int *out)
{
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return 0;
    *out = (int)n;
    return 1;
}

static int parse_float(const char *v, float *out)
{
    char *end = NULL;
    float f = strtof(v, &end);
    if (end == v) return 0;
    *out = f;
    return 1;
}

static void sanitise(vt3_touch_cfg *c)
{
    if (c->layout < 0 || c->layout >= VT3_LAYOUT_COUNT) c->layout = VT3_LAYOUT_STANDARD;
    if (c->profile < 0 || c->profile >= VT3_PROFILE_COUNT) c->profile = VT3_PROFILE_DEFAULT;
    c->locked = c->locked ? 1 : 0;
    c->swipe_enabled = c->swipe_enabled ? 1 : 0;
    c->swipe_labels = c->swipe_labels ? 1 : 0;
    c->swipe_charge = c->swipe_charge ? 1 : 0;
    c->swipe_min_dist = clampf(c->swipe_min_dist, 0.02f, 0.50f);
    for (int i = 0; i < 4; i++)
        if (c->swipe_map[i] < VT3_TC_A || c->swipe_map[i] > VT3_TC_Y) c->swipe_map[i] = VT3_TC_A + i;

    for (int l = 0; l < VT3_LAYOUT_COUNT; l++) {
        for (int k = 0; k < VT3_TC_COUNT; k++) {
            vt3_touch_ctl_cfg *t = &c->ctl[l][k];
            if (t->x >= 0.0f) t->x = clampf(t->x, -1.0f, 1.0f);
            else t->x = -1.0f;
            if (t->y >= 0.0f) t->y = clampf(t->y, -1.0f, 1.0f);
            else t->y = -1.0f;
            t->size = clampf(t->size, 0.5f, 2.5f);
            t->opacity = clampf(t->opacity, 0.15f, 1.0f);
            t->visible = t->visible ? 1 : 0;
            t->removed = t->removed ? 1 : 0;
        }
    }
    for (int i = 0; i < VT3_PROFILE_COUNT; i++) {
        vt3_stick_cfg *p = &c->prof[i];
        p->deadzone = clampf(p->deadzone, 0.0f, 0.60f);
        p->sensitivity = clampf(p->sensitivity, 0.5f, 2.0f);
        p->range = clampf(p->range, 0.5f, 1.5f);
        p->sens_x = clampf(p->sens_x, 0.5f, 2.0f);
        p->sens_y = clampf(p->sens_y, 0.5f, 2.0f);
        p->progressive = p->progressive ? 1 : 0;
        p->floating = p->floating ? 1 : 0;
        p->smooth_return = p->smooth_return ? 1 : 0;
        p->invert_x = p->invert_x ? 1 : 0;
        p->invert_y = p->invert_y ? 1 : 0;
    }
}

int vt3_touch_cfg_load(vt3_touch_cfg *cfg, const char *path)
{
    if (!cfg || !path) return -2;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char buf[VT3_TOUCH_CFG_MAX + 1];
    size_t n = fread(buf, 1, VT3_TOUCH_CFG_MAX, f);
    fclose(f);
    buf[n] = 0;

    vt3_touch_cfg tmp = *cfg;              /* parse into a copy: a bad file leaves cfg alone */
    int version = -1;
    int lines = 0;

    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        if (*s == 0 || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) return -2;                /* a line that is not key=value: not our file */
        *eq = 0;
        char *key = s, *val = eq + 1;
        size_t klen = strlen(key);
        while (klen && (key[klen - 1] == ' ' || key[klen - 1] == '\t')) key[--klen] = 0;
        while (*val == ' ' || *val == '\t') val++;
        size_t vlen = strlen(val);
        while (vlen && (val[vlen - 1] == '\r' || val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) val[--vlen] = 0;
        lines++;

        int iv;
        float fv;
        if (klen == 7 && memcmp(key, "version", 7) == 0) {
            if (!parse_int(val, &iv)) return -2;
            version = iv;
        } else if (klen == 6 && memcmp(key, "layout", 6) == 0) {
            int i = name_index(LAYOUT_NAME, VT3_LAYOUT_COUNT, val, vlen);
            if (i >= 0) tmp.layout = i;
        } else if (klen == 7 && memcmp(key, "profile", 7) == 0) {
            int i = name_index(PROFILE_NAME, VT3_PROFILE_COUNT, val, vlen);
            if (i >= 0) tmp.profile = i;
        } else if (klen == 4 && memcmp(key, "lock", 4) == 0) {
            if (parse_int(val, &iv)) tmp.locked = iv;
        } else if (klen > 6 && memcmp(key, "swipe.", 6) == 0) {
            const char *k = key + 6;
            size_t kl = klen - 6;
            if (kl == 7 && memcmp(k, "enabled", 7) == 0) { if (parse_int(val, &iv)) tmp.swipe_enabled = iv; }
            else if (kl == 6 && memcmp(k, "labels", 6) == 0) { if (parse_int(val, &iv)) tmp.swipe_labels = iv; }
            else if (kl == 6 && memcmp(k, "charge", 6) == 0) { if (parse_int(val, &iv)) tmp.swipe_charge = iv; }
            else if (kl == 8 && memcmp(k, "min_dist", 8) == 0) { if (parse_float(val, &fv)) tmp.swipe_min_dist = fv; }
            else if (kl > 4 && memcmp(k, "map.", 4) == 0) {
                int d = name_index(DIR_NAME, 4, k + 4, kl - 4);
                int ci = name_index(CTL_NAME, VT3_TC_COUNT, val, vlen);
                if (d >= 0 && ci >= VT3_TC_A && ci <= VT3_TC_Y) tmp.swipe_map[d] = ci;
            }
        } else if (klen > 2 && memcmp(key, "l.", 2) == 0) {
            /* l.<layout>.<control>.<property> */
            char *dot = strchr(key + 2, '.');
            if (!dot) continue;
            int l = name_index(LAYOUT_NAME, VT3_LAYOUT_COUNT, key + 2, (size_t)(dot - (key + 2)));
            if (l < 0) continue;
            char *dot2 = strchr(dot + 1, '.');
            if (!dot2) continue;
            int ci = name_index(CTL_NAME, VT3_TC_COUNT, dot + 1, (size_t)(dot2 - (dot + 1)));
            if (ci < 0) continue;
            const char *p = dot2 + 1;
            size_t pl = strlen(p);
            vt3_touch_ctl_cfg *t = &tmp.ctl[l][ci];
            if (pl == 1 && *p == 'x') { if (parse_float(val, &fv)) t->x = fv; }
            else if (pl == 1 && *p == 'y') { if (parse_float(val, &fv)) t->y = fv; }
            else if (pl == 4 && memcmp(p, "size", 4) == 0) { if (parse_float(val, &fv)) t->size = fv; }
            else if (pl == 7 && memcmp(p, "opacity", 7) == 0) { if (parse_float(val, &fv)) t->opacity = fv; }
            else if (pl == 7 && memcmp(p, "visible", 7) == 0) { if (parse_int(val, &iv)) t->visible = iv; }
            else if (pl == 7 && memcmp(p, "removed", 7) == 0) { if (parse_int(val, &iv)) t->removed = iv; }
        } else if (klen > 2 && memcmp(key, "p.", 2) == 0) {
            /* p.<profile>.<property> */
            char *dot = strchr(key + 2, '.');
            if (!dot) continue;
            int pi = name_index(PROFILE_NAME, VT3_PROFILE_COUNT, key + 2, (size_t)(dot - (key + 2)));
            if (pi < 0) continue;
            const char *p = dot + 1;
            size_t pl = strlen(p);
            vt3_stick_cfg *q = &tmp.prof[pi];
            if (pl == 8 && memcmp(p, "deadzone", 8) == 0) { if (parse_float(val, &fv)) q->deadzone = fv; }
            else if (pl == 11 && memcmp(p, "sensitivity", 11) == 0) { if (parse_float(val, &fv)) q->sensitivity = fv; }
            else if (pl == 5 && memcmp(p, "range", 5) == 0) { if (parse_float(val, &fv)) q->range = fv; }
            else if (pl == 11 && memcmp(p, "progressive", 11) == 0) { if (parse_int(val, &iv)) q->progressive = iv; }
            else if (pl == 8 && memcmp(p, "floating", 8) == 0) { if (parse_int(val, &iv)) q->floating = iv; }
            else if (pl == 6 && memcmp(p, "smooth", 6) == 0) { if (parse_int(val, &iv)) q->smooth_return = iv; }
            else if (pl == 7 && memcmp(p, "invertx", 7) == 0) { if (parse_int(val, &iv)) q->invert_x = iv; }
            else if (pl == 7 && memcmp(p, "inverty", 7) == 0) { if (parse_int(val, &iv)) q->invert_y = iv; }
            else if (pl == 5 && memcmp(p, "sensx", 5) == 0) { if (parse_float(val, &fv)) q->sens_x = fv; }
            else if (pl == 5 && memcmp(p, "sensy", 5) == 0) { if (parse_float(val, &fv)) q->sens_y = fv; }
        }
        /* anything else: a key from a newer version, ignored on purpose */
    }

    if (version < 0 || lines == 0) return -2;
    if (version > VT3_TOUCH_CFG_VERSION) return -2;   /* written by a newer app: keep our own */
    tmp.version = VT3_TOUCH_CFG_VERSION;
    sanitise(&tmp);
    *cfg = tmp;
    return 0;
}

int vt3_touch_cfg_save(const vt3_touch_cfg *cfg, const char *path)
{
    if (!cfg || !path) return -1;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = 1;
    ok &= fprintf(f, "version=%d\n", VT3_TOUCH_CFG_VERSION) > 0;
    ok &= fprintf(f, "layout=%s\n", LAYOUT_NAME[cfg->layout >= 0 && cfg->layout < VT3_LAYOUT_COUNT
                                              ? cfg->layout : VT3_LAYOUT_STANDARD]) > 0;
    ok &= fprintf(f, "profile=%s\n", PROFILE_NAME[cfg->profile >= 0 && cfg->profile < VT3_PROFILE_COUNT
                                                ? cfg->profile : VT3_PROFILE_DEFAULT]) > 0;
    ok &= fprintf(f, "lock=%d\n", cfg->locked ? 1 : 0) > 0;
    ok &= fprintf(f, "swipe.enabled=%d\nswipe.labels=%d\nswipe.charge=%d\nswipe.min_dist=%.3f\n",
                  cfg->swipe_enabled ? 1 : 0, cfg->swipe_labels ? 1 : 0, cfg->swipe_charge ? 1 : 0,
                  cfg->swipe_min_dist) > 0;
    for (int d = 0; d < 4; d++)
        ok &= fprintf(f, "swipe.map.%s=%s\n", DIR_NAME[d], CTL_NAME[cfg->swipe_map[d]]) > 0;
    for (int l = 0; l < VT3_LAYOUT_COUNT; l++) {
        for (int k = 0; k < VT3_TC_COUNT; k++) {
            const vt3_touch_ctl_cfg *t = &cfg->ctl[l][k];
            ok &= fprintf(f, "l.%s.%s.x=%.4f\nl.%s.%s.y=%.4f\nl.%s.%s.size=%.3f\n"
                             "l.%s.%s.opacity=%.3f\nl.%s.%s.visible=%d\nl.%s.%s.removed=%d\n",
                          LAYOUT_NAME[l], CTL_NAME[k], t->x, LAYOUT_NAME[l], CTL_NAME[k], t->y,
                          LAYOUT_NAME[l], CTL_NAME[k], t->size, LAYOUT_NAME[l], CTL_NAME[k], t->opacity,
                          LAYOUT_NAME[l], CTL_NAME[k], t->visible ? 1 : 0,
                          LAYOUT_NAME[l], CTL_NAME[k], t->removed ? 1 : 0) > 0;
        }
    }
    for (int i = 0; i < VT3_PROFILE_COUNT; i++) {
        const vt3_stick_cfg *p = &cfg->prof[i];
        ok &= fprintf(f, "p.%s.deadzone=%.3f\np.%s.sensitivity=%.3f\np.%s.range=%.3f\n"
                         "p.%s.progressive=%d\np.%s.floating=%d\np.%s.smooth=%d\n"
                         "p.%s.invertx=%d\np.%s.inverty=%d\np.%s.sensx=%.3f\np.%s.sensy=%.3f\n",
                      PROFILE_NAME[i], p->deadzone, PROFILE_NAME[i], p->sensitivity,
                      PROFILE_NAME[i], p->range, PROFILE_NAME[i], p->progressive,
                      PROFILE_NAME[i], p->floating, PROFILE_NAME[i], p->smooth_return,
                      PROFILE_NAME[i], p->invert_x, PROFILE_NAME[i], p->invert_y,
                      PROFILE_NAME[i], p->sens_x, PROFILE_NAME[i], p->sens_y) > 0;
    }
    if (fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(tmp); return -1; }
    remove(path);
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}
