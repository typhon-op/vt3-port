package net.vt3port.game;

import android.app.ActivityManager;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.PowerManager;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.WindowManager;

import java.io.File;

/**
 * Resolves the graphics settings into the marker files the runtime reads at start-up.
 *
 * The ladder starts at the panel: a flagship renders at its own resolution with 16x anisotropy and
 * a 60 fps cap; a low-end Helio renders at 720p with 2x and 30 fps. A preset is only a set of
 * defaults -- Low, Medium, High or the device's own Auto -- and every option has its own override
 * that wins over the preset. Touching an individual option puts the preset into Custom rather than
 * silently keeping a preset name that no longer describes the settings.
 *
 * Nothing here decides by brand: the tier comes from the SoC model Android reports, then memory,
 * core count and low-RAM status can only lower it, and the runtime's own frame statistics are what
 * actually keeps a device comfortable. A MediaTek Dimensity and a Snapdragon of the same class get
 * the same starting point.
 *
 * Markers written (all optional, all read in android_entry.c):
 *   preset, override_fps, override_res, override_aniso, override_scale,
 *   override_shadows, override_widescreen, thermal_guard        what the player chose
 *   fps_cap, render_h, aniso, render_scale_pct, no_vsm,
 *   no_widescreen                                               what the runtime uses
 */
public final class GraphicsProfile {
    private static final String TAG = "VT3Profile";

    /* Tiers, best first. */
    public static final int ULTRA = 0, HIGH = 1, BALANCED = 2, PERFORMANCE = 3, LOW = 4, MINIMUM = 5;
    public static final String[] TIER_NAME = { "Ultra", "High", "Balanced", "Performance", "Low", "Minimum" };

    /* Defaults per tier: render height (0 = the panel's own), frame cap, anisotropy. */
    static final int[] TIER_H = { 0, 0, 1080, 900, 720, 540 };
    static final int[] TIER_FPS = { 60, 60, 60, 60, 30, 30 };
    static final int[] TIER_ANISO = { 16, 16, 8, 4, 2, 1 };

    /* Low, Medium and High pin a tier; Auto follows the device and Custom keeps the overrides. */
    public static final String[] PRESET_IDS = { "auto", "low", "medium", "high", "custom" };

    /* The frame limits the runtime is tested with. 0 means uncapped (the panel's own rate). */
    public static final int[] FPS = { -1, 30, 35, 40, 45, 50, 55, 60, 0 };

    /* Render heights. 0 is the panel's own resolution, which is what the top tiers use. */
    public static final int[] RES = { -1, 0, 1440, 1220, 1080, 900, 720, 540 };

    public static final int[] ANISO = { -1, 1, 2, 4, 8, 16 };

    /*
     * Every label is its own string resource, named after the value it names, and it is looked up
     * from that value: fpsLabelId(30) and resLabelId(1080) each find their own id. A translation can
     * therefore be reordered or rewritten without a label ever landing on another stored value,
     * which a list indexed by position could not promise.
     */
    static int presetLabelId(String preset) {
        switch (preset) {
            case "low": return R.string.preset_low;
            case "medium": return R.string.preset_medium;
            case "high": return R.string.preset_high;
            case "custom": return R.string.preset_custom;
            default: return R.string.preset_auto;
        }
    }

    static int fpsLabelId(int fps) {
        switch (fps) {
            case 30: return R.string.fps_30;
            case 35: return R.string.fps_35;
            case 40: return R.string.fps_40;
            case 45: return R.string.fps_45;
            case 50: return R.string.fps_50;
            case 55: return R.string.fps_55;
            case 60: return R.string.fps_60;
            case 0: return R.string.fps_uncapped;
            default: return R.string.fps_auto;
        }
    }

    static int resLabelId(int height) {
        switch (height) {
            case 0: return R.string.res_native;
            case 1440: return R.string.res_1440p;
            case 1220: return R.string.res_1220p;
            case 1080: return R.string.res_1080p;
            case 900: return R.string.res_900p;
            case 720: return R.string.res_720p;
            case 540: return R.string.res_540p;
            default: return R.string.res_auto;
        }
    }

    static int anisoLabelId(int aniso) {
        switch (aniso) {
            case 1: return R.string.aniso_off;
            case 2: return R.string.aniso_2x;
            case 4: return R.string.aniso_4x;
            case 8: return R.string.aniso_8x;
            case 16: return R.string.aniso_16x;
            default: return R.string.aniso_auto;
        }
    }

    /** The preset names in PRESET_IDS order, for the picker. */
    public static String[] presetLabels(Context c) {
        String[] out = new String[PRESET_IDS.length];
        for (int i = 0; i < PRESET_IDS.length; i++) out[i] = c.getString(presetLabelId(PRESET_IDS[i]));
        return out;
    }

    /** The frame-rate names in FPS order, for the picker. */
    public static String[] fpsLabels(Context c) {
        String[] out = new String[FPS.length];
        for (int i = 0; i < FPS.length; i++) out[i] = c.getString(fpsLabelId(FPS[i]));
        return out;
    }

    /** The resolution names in RES order, for the picker. */
    public static String[] resLabels(Context c) {
        String[] out = new String[RES.length];
        for (int i = 0; i < RES.length; i++) out[i] = c.getString(resLabelId(RES[i]));
        return out;
    }

    /** The anisotropy names in ANISO order, for the picker. */
    public static String[] anisoLabels(Context c) {
        String[] out = new String[ANISO.length];
        for (int i = 0; i < ANISO.length; i++) out[i] = c.getString(anisoLabelId(ANISO[i]));
        return out;
    }

    /** The thermal status name, by the status itself. */
    public static String thermalLabel(Context c, int status) {
        switch (status) {
            case PowerManager.THERMAL_STATUS_NONE: return c.getString(R.string.thermal_cool);
            case PowerManager.THERMAL_STATUS_LIGHT: return c.getString(R.string.thermal_warm);
            case PowerManager.THERMAL_STATUS_MODERATE: return c.getString(R.string.thermal_moderate);
            case PowerManager.THERMAL_STATUS_SEVERE: return c.getString(R.string.thermal_hot);
            case PowerManager.THERMAL_STATUS_CRITICAL: return c.getString(R.string.thermal_critical);
            default: return c.getString(R.string.thermal_emergency);
        }
    }

    /** The device tier name, for the settings screen. */
    public static String tierLabel(Context c, int tier) {
        switch (tier) {
            case ULTRA: return c.getString(R.string.tier_ultra);
            case HIGH: return c.getString(R.string.tier_high);
            case BALANCED: return c.getString(R.string.tier_balanced);
            case PERFORMANCE: return c.getString(R.string.tier_performance);
            case LOW: return c.getString(R.string.tier_low);
            default: return c.getString(R.string.tier_minimum);
        }
    }

    private GraphicsProfile() {}

    /** The tier a preset pins, or -1 for Auto/Custom (both follow the device). */
    public static int presetTier(String preset) {
        if (preset == null) return -1;
        switch (preset) {
            case "high":
            case "quality": return ULTRA;
            case "medium":
            case "balanced": return BALANCED;
            case "performance": return PERFORMANCE;
            case "low": return LOW;
            default: return -1;
        }
    }

    /**
     * Classify this device. The SoC model Android reports is the starting point; memory, cores and
     * low-RAM status can only lower the result, never raise it. Unknown hardware starts Balanced.
     */
    public static int classify(Context c) {
        String soc = "";
        if (Build.VERSION.SDK_INT >= 31) {
            soc = (nz(Build.SOC_MANUFACTURER) + " " + nz(Build.SOC_MODEL)).trim();
        }
        if (soc.isEmpty()) soc = nz(Build.HARDWARE);
        soc = (soc + " " + nz(Build.HARDWARE) + " " + nz(Build.BOARD)).toLowerCase();

        int tier = BALANCED;
        if (matches(soc, "sm8750", "sm8650", "sm8550", "sm8475", "sm8450", "sm8350",
                "mt6991", "mt6989", "mt6985", "mt6983", "exynos2400", "exynos2200",
                "tensor g3", "tensor g4")) {
            tier = ULTRA;
        } else if (matches(soc, "sm8250", "sm7675", "sm7550", "sm7450", "sm7325", "sm7250",
                "mt6897", "mt6895", "mt6886", "mt6879", "mt6885", "mt6835",
                "exynos1480", "exynos1380", "tensor")) {
            tier = HIGH;
        } else if (matches(soc, "mt6789", "mt6785", "mt6769", "mt6768", "mt6765",
                "sm6225", "sm6115", "sm4350", "mt6833", "mt6853", "exynos850", "exynos1330")) {
            tier = LOW;
        } else if (matches(soc, "mt6739", "mt6761", "mt6762", "msm8953", "sc9863", "ums512")) {
            tier = MINIMUM;
        }
        /* A small or low-RAM device cannot hold a high tier whatever the SoC name says. */
        int cores = Runtime.getRuntime().availableProcessors();
        long mem = totalMem(c);
        if (cores <= 4) tier = Math.max(tier, LOW);
        if (mem > 0 && mem < 3L << 30) tier = Math.max(tier, LOW);
        if (mem > 0 && mem < 2L << 30) tier = Math.max(tier, MINIMUM);
        if (lowRam(c)) tier = Math.max(tier, LOW);
        return tier;
    }

    private static String nz(String s) {
        return s == null ? "" : s;
    }

    private static boolean matches(String haystack, String... needles) {
        for (String n : needles) if (haystack.contains(n)) return true;
        return false;
    }

    private static long totalMem(Context c) {
        ActivityManager.MemoryInfo info = new ActivityManager.MemoryInfo();
        ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        if (am == null) return 0;
        am.getMemoryInfo(info);
        return info.totalMem;
    }

    private static boolean lowRam(Context c) {
        ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        return am != null && am.isLowRamDevice();
    }

    /** What the settings screen says about the hardware: real values, no guessed GPU names. */
    public static String describeDevice(Context c) {
        StringBuilder s = new StringBuilder();
        String maker = Build.VERSION.SDK_INT >= 31 ? nz(Build.SOC_MANUFACTURER) : "";
        String model = Build.VERSION.SDK_INT >= 31 ? nz(Build.SOC_MODEL) : "";
        if (!model.isEmpty()) {
            s.append(maker.isEmpty() ? "" : maker + " ").append(model);
        } else {
            s.append(nz(Build.HARDWARE));
        }
        long mem = totalMem(c);
        if (mem > 0) s.append("  ·  ").append(String.format(java.util.Locale.US, "%.1f GB RAM",
                mem / 1073741824.0));
        s.append("  ·  ").append(Runtime.getRuntime().availableProcessors()).append(" cores");
        String vk = vulkanInfo(c);
        if (vk != null) s.append("\n").append(vk);
        return s.toString();
    }

    /** The Vulkan level and version the platform advertises, which is what the renderer can use. */
    public static String vulkanInfo(Context c) {
        PackageManager pm = c.getPackageManager();
        if (!pm.hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL)) return null;
        int level = -1, version = -1;
        try {
            level = pm.getSystemAvailableFeatures().length; /* keep the API call below cheap */
            for (android.content.pm.FeatureInfo f : pm.getSystemAvailableFeatures()) {
                if (PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL.equals(f.name)) level = f.version;
                if (PackageManager.FEATURE_VULKAN_HARDWARE_VERSION.equals(f.name)) version = f.version;
            }
        } catch (RuntimeException e) {
            return null;
        }
        StringBuilder s = new StringBuilder("Vulkan");
        if (version > 0) {
            s.append(' ').append((version >> 22) & 0x3ff).append('.')
                    .append((version >> 12) & 0x3ff).append('.')
                    .append(version & 0xfff);
        }
        if (level >= 0) s.append(" (hardware level ").append(level).append(')');
        return s.toString();
    }

    /** Resolve the preset, the overrides and the thermal status into the markers the runtime reads. */
    public static void apply(Context c, File root) {
        String preset = Markers.read(root, "preset", "auto");
        int base = presetTier(preset);
        int tier = base >= 0 ? base : classify(c);

        boolean guard = Markers.flag(root, "thermal_guard", true);
        int thermal = Markers.intOr(root, "thermal", 0);
        if (!guard) thermal = 0;

        /* Thermal: nothing at NONE/LIGHT, one rung at SEVERE, two at CRITICAL and beyond. The rung
         * moves the resolution for the next launch only, which is the gradual lever; the runtime
         * lowers anisotropy while a match runs. */
        int rung = thermal >= 4 ? 2 : thermal >= 3 ? 1 : 0;
        int hi = Math.min(TIER_NAME.length - 1, tier + rung);

        Integer ofps = Markers.readInt(root, "override_fps", null);
        Integer ores = Markers.readInt(root, "override_res", null);
        Integer oaniso = Markers.readInt(root, "override_aniso", null);
        Integer oscale = Markers.readInt(root, "override_scale", null);

        int fps = ofps != null ? ofps : TIER_FPS[tier];
        int height = ores != null ? ores : TIER_H[hi];
        int aniso = oaniso != null ? oaniso : TIER_ANISO[tier];
        int scale = oscale != null ? oscale : 100;
        /* The ladder's top two tiers both render at the panel's own height, so a thermal rung there
         * has to move the scale instead: 85 % at SEVERE, 75 % at CRITICAL. Never applied to a height
         * the player pinned themselves. */
        if (ores == null && height == 0 && rung > 0) scale = Math.min(scale, rung >= 2 ? 75 : 85);
        /* The runtime lowers anisotropy under heat; the marker already reflects the cap so a
         * restart does not undo it. */
        if (thermal >= 2) aniso = Math.min(aniso, 8);
        if (thermal >= 3) aniso = Math.min(aniso, 4);
        if (thermal >= 4) aniso = Math.min(aniso, 2);

        /* Shadows (the game's variance shadow maps) follow the preset unless overridden: the two
         * top tiers keep them, everything below renders without them. */
        boolean shadows = Markers.flag(root, "override_shadows", tier <= HIGH);
        /* Widescreen is on unless the player turned it off. */
        boolean wide = Markers.flag(root, "override_widescreen", true);

        Markers.write(root, "fps_cap", Integer.toString(fps));
        Markers.write(root, "render_h", height > 0 ? Integer.toString(height) : null);
        Markers.write(root, "aniso", Integer.toString(aniso));
        Markers.write(root, "render_scale_pct", Integer.toString(scale));
        Markers.write(root, "no_vsm", shadows ? null : "1");
        Markers.write(root, "no_widescreen", wide ? null : "1");
        DiagLog.i(TAG, "tier " + TIER_NAME[tier] + " preset " + preset + " thermal " + thermal
                + " guard " + guard + " -> fps " + fps + " height " + (height > 0 ? height : "native")
                + " aniso " + aniso + " scale " + scale + "% shadows " + shadows + " wide " + wide);
    }

    /** Panel size and rate, for the settings screen. */
    public static String describeDisplay(Context c) {
        WindowManager wm = (WindowManager) c.getSystemService(Context.WINDOW_SERVICE);
        if (wm == null) return "";
        Display d = wm.getDefaultDisplay();
        DisplayMetrics m = new DisplayMetrics();
        d.getRealMetrics(m);
        int w = Math.max(m.widthPixels, m.heightPixels), h = Math.min(m.widthPixels, m.heightPixels);
        return w + "x" + h + " @ " + Math.round(d.getRefreshRate()) + " Hz";
    }

    /**
     * Thermal status names, in the order {@link #thermalIndex} returns. The settings screen shows
     * the translated list; the log and the report use {@link #thermalName}, which stays English.
     */
    public static final String[] THERMAL_NAME = {
            "cool", "warm", "moderate", "hot", "critical", "emergency"
    };

    /** Index into {@link #THERMAL_NAME} for a PowerManager thermal status. */
    public static int thermalIndex(int status) {
        switch (status) {
            case PowerManager.THERMAL_STATUS_NONE: return 0;
            case PowerManager.THERMAL_STATUS_LIGHT: return 1;
            case PowerManager.THERMAL_STATUS_MODERATE: return 2;
            case PowerManager.THERMAL_STATUS_SEVERE: return 3;
            case PowerManager.THERMAL_STATUS_CRITICAL: return 4;
            default: return 5;
        }
    }

    public static String thermalName(int status) {
        return THERMAL_NAME[thermalIndex(status)];
    }

    /* ---- marker access, kept for the diagnostics report and the tests ---- */

    /** A marker's text, or def when it is absent or empty. */
    public static String read(File root, String name, String def) {
        return Markers.read(root, name, def);
    }

    /** A marker as an integer, or def when it is absent or not a number. */
    public static int readInt(File root, String name, int def) {
        return Markers.intOr(root, name, def);
    }

    /** The current thermal status, or -1 when the platform has no thermal service. */
    public static int thermalStatus(Context c) {
        PowerManager pm = (PowerManager) c.getSystemService(Context.POWER_SERVICE);
        return pm != null ? pm.getCurrentThermalStatus() : -1;
    }
}
