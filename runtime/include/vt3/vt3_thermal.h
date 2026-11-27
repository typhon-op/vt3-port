/*
 * Thermal sensor readings, for the performance overlay. `root` is "/sys" in the app and a fake tree
 * in the host test, so the parsing is testable without a device.
 */
#ifndef VT3_THERMAL_H
#define VT3_THERMAL_H

#include <stddef.h>

/* Normalises a kernel temperature: millidegrees or decidegrees to whole degrees C. */
int vt3_thermal_degrees(long raw);

/* 1 and *deg set when a plausible SoC/CPU zone was read; src receives "zone (type)" for the overlay
 * so the reading is attributable. 0 when nothing readable or plausible was found. */
int vt3_thermal_cpu(const char *root, int *deg, char *src, size_t cap);

/* 1 and *deg set when the battery sensor was read; src receives its path. */
int vt3_thermal_battery(const char *root, int *deg, char *src, size_t cap);

#endif
