/*
 * Thermal readings from the kernel, for the performance overlay.
 *
 * The overlay used to depend on markers written by a Java activity, which does not exist while a
 * match runs, so it read "Unavailable" during play. These helpers read the sensors directly. They
 * take the sysfs root as an argument so the parsing can be tested on a host against a fake tree,
 * which is the only part of this that can be verified without a device: whether an Android kernel
 * and SELinux actually let the app read a zone is a device question.
 *
 * Format rules found on Android kernels:
 *   thermal_zoneN/temp        millidegrees C on most, decidegrees C on some older SoCs
 *   power_supply/<name>/temp  tenths of a degree C
 * Both are normalised here, and a value outside a plausible human range is rejected rather than
 * shown, so a bogus number can never reach the overlay.
 */
#include "vt3/vt3_thermal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -40..130 C covers every phone in any climate; anything outside means the unit or the sensor is
 * not what we think it is. */
#define VT3_THERMAL_MIN (-40)
#define VT3_THERMAL_MAX 130

static int read_number(const char *path, long *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[64] = { 0 };
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0) return 0;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf) return 0;
    *out = v;
    return 1;
}

static int read_text(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    if (n == 0) return 0;
    out[n] = 0;
    for (size_t i = 0; i < n; i++) {
        if (out[i] == '\n' || out[i] == '\r') {
            out[i] = 0;
            break;
        }
    }
    return 1;
}

int vt3_thermal_degrees(long raw)
{
    if (raw > 1000 || raw < -1000) return (int)(raw / 1000);
    return (int)(raw / 10);
}

int vt3_thermal_cpu(const char *root, int *deg, char *src, size_t cap)
{
    /* Substrings of the zone types that are the SoC or a CPU cluster on the kernels seen so far. */
    static const char *hints[] = { "cpu", "soc", "tsens", "ap", "cluster", "big", "little", "skin" };
    char zone[192], path[256], type[96];
    for (int i = 0; i < 64; i++) {
        snprintf(zone, sizeof zone, "%s/class/thermal/thermal_zone%d", root, i);
        snprintf(path, sizeof path, "%s/temp", zone);
        long raw;
        if (!read_number(path, &raw)) continue;
        snprintf(path, sizeof path, "%s/type", zone);
        if (!read_text(path, type, sizeof type)) type[0] = 0;
        for (int h = 0; h < (int)(sizeof hints / sizeof hints[0]); h++) {
            if (!strstr(type, hints[h])) continue;
            int c = vt3_thermal_degrees(raw);
            if (c > VT3_THERMAL_MIN && c < VT3_THERMAL_MAX) {
                *deg = c;
                snprintf(src, cap, "%s (%.40s)", zone, type);
                return 1;
            }
            break;   /* this zone matched but is not plausible; try the next one */
        }
    }
    return 0;
}

int vt3_thermal_battery(const char *root, int *deg, char *src, size_t cap)
{
    static const char *names[] = { "battery", "bms", "Battery" };
    char path[256];
    for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++) {
        snprintf(path, sizeof path, "%s/class/power_supply/%s/temp", root, names[i]);
        long raw;
        if (!read_number(path, &raw)) continue;
        int c = vt3_thermal_degrees(raw);
        if (c > VT3_THERMAL_MIN && c < VT3_THERMAL_MAX) {
            *deg = c;
            snprintf(src, cap, "%s", path);
            return 1;
        }
    }
    return 0;
}
