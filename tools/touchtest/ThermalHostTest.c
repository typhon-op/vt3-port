/* Host checks for the overlay's thermal parsing, against a fake sysfs tree. */
#include "vt3/vt3_thermal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int checks, failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL: cannot create %s\n", path); failures++; return; }
    fputs(text, f);
    fclose(f);
}

static void zone(const char *root, int i, const char *type, const char *temp)
{
    char dir[256], path[320];
    snprintf(dir, sizeof dir, "%s/class/thermal/thermal_zone%d", root, i);
    mkdir(dir, 0777);
    snprintf(path, sizeof path, "%s/type", dir);
    write_file(path, type);
    snprintf(path, sizeof path, "%s/temp", dir);
    write_file(path, temp);
}

static void fresh(const char *root)
{
    char cmd[320];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s/class/thermal' '%s/class/power_supply'", root, root, root);
    if (system(cmd) != 0) printf("note: could not reset %s\n", root);
}

int main(void)
{
    const char *root = "/tmp/vt3_thermal_test";
    int deg = 0;
    char src[160];

    /* unit normalisation */
    ok(vt3_thermal_degrees(42000) == 42, "millidegrees 42000 -> 42");
    ok(vt3_thermal_degrees(420) == 42, "decidegrees 420 -> 42");
    ok(vt3_thermal_degrees(-5000) == -5, "negative millidegrees -5000 -> -5");
    ok(vt3_thermal_degrees(355) == 35, "tenths 355 -> 35");

    /* a battery zone is not a CPU zone; the CPU zone after it is picked, with its source named */
    fresh(root);
    zone(root, 0, "mtktsbattery", "30000\n");
    zone(root, 1, "cpu-0-0", "41500\n");
    deg = 0; src[0] = 0;
    ok(vt3_thermal_cpu(root, &deg, src, sizeof src) == 1, "cpu zone found past a battery zone");
    ok(deg == 41, "cpu reading 41500 -> 41 C");
    ok(strstr(src, "thermal_zone1") != NULL && strstr(src, "cpu-0-0") != NULL, "cpu source names the zone");

    /* decidegrees zone */
    fresh(root);
    zone(root, 0, "soc-thermal", "420\n");
    deg = 0;
    ok(vt3_thermal_cpu(root, &deg, src, sizeof src) == 1 && deg == 42, "decidegrees zone -> 42 C");

    /* implausible value in a matching zone is rejected, the next matching zone is used */
    fresh(root);
    zone(root, 0, "cpu", "999999\n");
    zone(root, 1, "cluster-big", "39000\n");
    deg = 0;
    ok(vt3_thermal_cpu(root, &deg, src, sizeof src) == 1 && deg == 39, "implausible 999 C skipped for 39 C");

    /* nothing readable: honest failure, not a made-up number */
    fresh(root);
    deg = 12345;
    ok(vt3_thermal_cpu(root, &deg, src, sizeof src) == 0, "empty tree reports no cpu reading");
    ok(deg == 12345, "no reading leaves the caller's value untouched");

    /* unreadable temp file (present zone, no value) is skipped */
    fresh(root);
    mkdir("/tmp/vt3_thermal_test/class/thermal/thermal_zone0", 0777);
    write_file("/tmp/vt3_thermal_test/class/thermal/thermal_zone0/type", "cpu\n");
    deg = 12345;
    ok(vt3_thermal_cpu(root, &deg, src, sizeof src) == 0, "zone without a temp file is skipped");

    /* battery sensor */
    fresh(root);
    mkdir("/tmp/vt3_thermal_test/class/power_supply/battery", 0777);
    write_file("/tmp/vt3_thermal_test/class/power_supply/battery/temp", "355\n");
    deg = 0;
    ok(vt3_thermal_battery(root, &deg, src, sizeof src) == 1 && deg == 35, "battery 355 -> 35 C");
    ok(strstr(src, "power_supply/battery/temp") != NULL, "battery source names its path");

    fresh(root);
    deg = 12345;
    ok(vt3_thermal_battery(root, &deg, src, sizeof src) == 0, "no battery sensor reports none");
    ok(deg == 12345, "no battery reading leaves the caller's value untouched");

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
