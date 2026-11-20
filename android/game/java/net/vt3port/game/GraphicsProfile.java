package net.vt3port.game;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Build;
import android.os.PowerManager;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.InputStream;

/**
 * Resolves the graphics settings into the marker files the runtime reads at start-up.
 *
 * The ladder starts at the panel: a flagship renders at its own resolution with 16x anisotropy and
 * a 60 fps cap, a Helio G85 at 720p with 2x and 30 fps. A preset is only a set of defaults; every
 * option has its own override, and an override wins over whatever the preset or the device tier
 * chose. Nothing here restricts a device by name alone: unknown hardware starts in the middle and
 * the runtime's own frame statistics decide whether it stays there.
 *
 * Written markers (all optional, all read in android_entry.c):
 *   preset, override_fps, override_res, override_aniso, override_scale   what the player chose
 *   fps_cap, render_h, aniso, render_scale_pct, thermal                  what the runtime uses
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

    public static final String[] PRESET_IDS = { "auto", "quality", "balanced", "performance", "low" };
    public static final String[] PRESET_LABELS = { "Auto (device)", "Quality", "Balanced", "Performance", "Low" };

    private GraphicsProfile() {}

    /** The tier a preset pins, or -1 for auto. */
    public static int presetTier(String preset) {
        if (preset == null) return -1;
        switch (preset) {
            case "quality": return ULTRA;
            case "balanced": return BALANCED;
            case "performance": return PERFORMANCE;
            case "low": return LOW;
            default: return -1;
        }
    }

    /** Classify this device. Unknown hardware starts at Balanced, never at Minimum. */
    public static int classify(Context c) {
        String soc = (Build.VERSION.SDK_INT >= 31 ? Build.SOC_MODEL : Build.HARDWARE) + " " + Build.HARDWARE;
        soc = soc.toLowerCase();
        int tier = BALANCED;
        if (matches(soc, "sm8650", "sm8550", "sm8475", "sm8450", "sm8350", "mt6985", "mt6983",
                "mt6989", "exynos2400", "exynos2200", "tensor g3", "tensor g4", "qti-mtp")) {
            tier = HIGH;
            if (matches(soc, "sm8650", "sm8550", "mt6985", "mt6989", "exynos2400")) tier = ULTRA;
        } else if (matches(soc, "sm8250", "sm7325", "sm7675", "sm7450", "mt6879", "mt6885",
                "mt6895", "mt6835", "exynos1380", "exynos1480", "tensor")) {
            tier = BALANCED;
        } else if (matches(soc, "mt6769", "mt6768", "mt6765", "sm6225", "sm6115", "sm4350",
                "mt6833", "mt6853", "exynos850", "exynos1330", "ums512")) {
            tier = LOW;
        } else if (matches(soc, "mt6739", "mt6761", "mt6762", "msm8953", "qcom", "sc9863")) {
            tier = MINIMUM;
        }
        /* A small or low-RAM device cannot hold a high tier whatever the SoC name says. */
        int cores = Runtime.getRuntime().availableProcessors();
        long mem = totalMem(c);
        if (cores <= 4) tier = Math.max(tier, LOW);
        if (mem > 0 && mem < 3L << 30) tier = Math.max(tier, LOW);
        if (mem > 0 && mem < 2L << 30) tier = Math.max(tier, MINIMUM);
        return tier;
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

    /** Resolve the preset, the overrides and the thermal status into the markers the runtime reads. */
    public static void apply(Context c, File root) {
        String preset = read(root, "preset", "auto");
        int base = presetTier(preset);
        int tier = base >= 0 ? base : classify(c);

        /* Thermal: nothing at NONE/LIGHT, one rung at SEVERE, two at CRITICAL and beyond. The rung
         * moves the resolution for the next launch only, which is the gradual lever; the runtime
         * lowers anisotropy while a match runs. */
        int thermal = readInt(root, "thermal", 0);
        int rung = thermal >= 4 ? 2 : thermal >= 3 ? 1 : 0;
        int hi = Math.min(TIER_NAME.length - 1, tier + rung);

        Integer ofps = override(root, "override_fps");
        Integer ores = override(root, "override_res");
        Integer oaniso = override(root, "override_aniso");
        Integer oscale = override(root, "override_scale");

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

        write(root, "fps_cap", Integer.toString(fps));
        write(root, "render_h", height > 0 ? Integer.toString(height) : null);
        write(root, "aniso", Integer.toString(aniso));
        write(root, "render_scale_pct", Integer.toString(scale));
        Log.i(TAG, "tier " + TIER_NAME[tier] + " preset " + preset + " thermal " + thermal
                + " -> fps " + fps + " height " + (height > 0 ? height : "native")
                + " aniso " + aniso + " scale " + scale + "%");
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

    public static String thermalName(int status) {
        switch (status) {
            case PowerManager.THERMAL_STATUS_NONE: return "cool";
            case PowerManager.THERMAL_STATUS_LIGHT: return "warm";
            case PowerManager.THERMAL_STATUS_MODERATE: return "moderate";
            case PowerManager.THERMAL_STATUS_SEVERE: return "hot";
            case PowerManager.THERMAL_STATUS_CRITICAL: return "critical";
            default: return "emergency";
        }
    }

    /* ---- marker files ------------------------------------------------------------- */

    static String read(File root, String name, String def) {
        File f = new File(root, name);
        if (!f.exists()) return def;
        try (InputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            byte[] b = new byte[256];
            for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n);
            String s = o.toString("UTF-8").trim();
            return s.isEmpty() ? def : s;
        } catch (IOException e) {
            return def;
        }
    }

    static Integer override(File root, String name) {
        String s = read(root, name, null);
        if (s == null) return null;
        try {
            return Integer.parseInt(s.trim());
        } catch (NumberFormatException e) {
            return null;
        }
    }

    public static int readInt(File root, String name, int def) {
        Integer v = override(root, name);
        return v != null ? v : def;
    }

    /** Write a marker, or delete it when value is null. */
    static void write(File root, String name, String value) {
        File f = new File(root, name);
        try {
            if (value == null) {
                f.delete();
                return;
            }
            try (FileOutputStream out = new FileOutputStream(f)) {
                out.write(value.getBytes("UTF-8"));
            }
        } catch (IOException e) {
            Log.w(TAG, "could not write " + name + ": " + e.getMessage());
        }
    }
}
