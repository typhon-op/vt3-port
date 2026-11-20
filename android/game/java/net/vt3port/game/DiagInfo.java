package net.vt3port.game;

import android.app.ActivityManager;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.WindowManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Collects the report's data. Everything here is a read of something that already exists: package
 * info, the game's marker files, the native metrics snapshot, the log. Nothing is inferred, and a
 * value that cannot be read says so.
 */
final class DiagInfo {
    private DiagInfo() {}

    static DiagData collect(Context c, DiagCore core) {
        DiagData d = new DiagData();
        File root = c.getExternalFilesDir(null);
        if (root == null) root = c.getFilesDir();
        Map<String, String> nativeMetrics = readMetrics(new File(root, "diag_metrics.txt"));

        fillApp(c, d, nativeMetrics);
        fillInstall(c, root, d, core);
        fillGraphics(c, root, d, nativeMetrics);
        fillAudio(d, nativeMetrics);

        if (core != null) {
            List<String> life = core.linesOf("lifecycle", 6000);
            if (life.size() > 60) life = life.subList(life.size() - 60, life.size());
            d.lifecycle.addAll(life);
            List<String> errs = core.errors(20000);
            if (errs.size() > 200) errs = errs.subList(errs.size() - 200, errs.size());
            d.errors.addAll(errs);
        }
        d.lastError = DiagLog.lastError();
        d.crashLog = DiagLog.readText(new File(DiagLog.diagnosticsDir(), "crash.txt"), 4000);
        d.previousSession = DiagLog.previousSession();
        d.generatedAt = System.currentTimeMillis();
        d.logBytes = core != null ? core.sizeBytes() : 0;
        d.droppedEntries = core != null ? core.dropped() : 0;

        d.app.put("Last known state", summarize(d, nativeMetrics));
        d.problems.addAll(problems(c, root, d, nativeMetrics));
        return d;
    }

    /* ---- A ---------------------------------------------------------------- */

    private static void fillApp(Context c, DiagData d, Map<String, String> m) {
        String version = "unknown", code = "unknown", updated = "unknown";
        try {
            PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            version = pi.versionName == null ? "unknown" : pi.versionName;
            code = Integer.toString(pi.versionCode);
            if (Build.VERSION.SDK_INT >= 28) code = Long.toString(pi.getLongVersionCode());
            updated = new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US)
                    .format(new Date(pi.lastUpdateTime));
        } catch (Exception e) {
            DiagLog.w("diagnostics", "package info unavailable: " + e.getMessage());
        }
        d.app.put("Application version", version);
        d.app.put("Build number", code);
        d.app.put("App last updated", updated);
        d.app.put("Android version", Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")");
        d.app.put("Device model", Build.MANUFACTURER + " " + Build.MODEL);
        d.app.put("CPU architecture", String.join(", ", Build.SUPPORTED_ABIS));
        d.app.put("Available RAM", ram(c));
        d.app.put("App startup", new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)
                .format(new Date(DiagLog.startedAt())));
        d.app.put("Process uptime", ((System.currentTimeMillis() - DiagLog.startedAt()) / 1000) + " s");
        d.app.put("Device uptime", (SystemClock.elapsedRealtime() / 1000) + " s");
        d.app.put("Session", DiagLog.previousSession().isEmpty() ? "started cleanly"
                : DiagLog.previousSession());
        d.app.put("Detailed logging", DiagLog.detail() ? "on" : "off");
        if (m.containsKey("log_dropped"))
            d.app.put("Log entries dropped", m.get("log_dropped") + " (queue full; frames were not blocked)");
        d.app.put("Native metrics snapshot",
                m.isEmpty() ? "unavailable: the renderer has not written one yet"
                        : new File(DiagLog.root(), "diag_metrics.txt").getAbsolutePath());
    }

    private static String ram(Context c) {
        ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        if (am == null) return "unavailable";
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(mi);
        return (mi.availMem >> 20) + " MB free of " + (mi.totalMem >> 20) + " MB";
    }

    /* ---- B ---------------------------------------------------------------- */

    private static void fillInstall(Context c, File root, DiagData d, DiagCore core) {
        File ok = new File(root, "install.ok");
        File gameRootMarker = new File(root, "game_root");
        String marker = readSmall(gameRootMarker);
        File data = marker != null && !marker.isEmpty() ? new File(marker) : new File(root, "data2");

        d.install.put("Installation status", ok.isFile() ? "installed" : "not installed");
        d.install.put("Install marker", ok.isFile() ? ok.getAbsolutePath() : "install.ok missing");
        String source = readSmall(new File(root, "source"));
        File picked = StorageAccess.sourcePath(c);
        d.install.put("Selected game source type",
                picked != null ? "unpacked game folder, read in place (all files access)"
                        : source != null && !source.isEmpty() ? source
                        : "disc image or picked folder, copied into app storage on install");
        d.install.put("Installation destination",
                StorageAccess.destPath(c) != null ? StorageAccess.destPath(c).getAbsolutePath()
                        : root.getAbsolutePath() + " (app storage)");
        d.install.put("Game data root", data.getAbsolutePath() + (data.isDirectory() ? "" : " (missing)"));

        File image = new File(root, "image.bin");
        d.install.put("Game image", image.isFile()
                ? (image.length() >> 20) + " MB, SHA-256 " + sha256(image) : "image.bin missing");
        File shaders = new File(root, "shaders.pack");
        d.install.put("Shader pack", shaders.isFile() ? (shaders.length() >> 10) + " KB" : "missing");

        File resource = new File(data, "Resource");
        int[] counts = count(data);
        d.install.put("Required file verification", resource.isDirectory()
                ? "Resource folder present; " + counts[0] + " files, " + (counts[1] >> 20) + " MB under the data root"
                : "FAILED: no Resource folder under the data root");
        d.install.put("Missing or corrupted files",
                resource.isDirectory() ? "none detected by the checks above"
                        : "the Resource folder the game opens at start-up is absent");
        d.install.put("Storage permission", StorageAccess.allFiles(c)
                ? "all files access granted"
                : "all files access not granted (the picker path needs none); "
                        + (StorageAccess.asked(c) ? "the player has seen the prompt" : "never asked"));
        d.install.put("App storage writable", StorageAccess.readable(root) ? "yes" : "NO");
        if (StorageAccess.destPath(c) != null && !StorageAccess.allFiles(c))
            d.install.put("Custom destination problem", "a custom folder is remembered but all files access is off");
        List<String> installErrors = core != null ? core.linesOf("install", 12000) : new ArrayList<>();
        d.install.put("Install errors", installErrors.isEmpty() ? "none recorded"
                : installErrors.size() + " line(s) in the error history below");
    }

    /* ---- C ---------------------------------------------------------------- */

    private static void fillGraphics(Context c, File root, DiagData d, Map<String, String> m) {
        d.graphics.put("GPU vendor", value(m.get("gpu_vendor"), "unavailable"));
        d.graphics.put("GPU renderer", value(m.get("gpu_name"), "unavailable"));
        d.graphics.put("Vulkan version", value(m.get("gpu_api"), "unavailable"));
        d.graphics.put("Vulkan extensions", value(m.get("gpu_extensions"), "unavailable"));
        d.graphics.put("Graphics preset", value(GraphicsProfile.read(root, "preset", "auto"), "auto"));
        d.graphics.put("Frame cap", value(GraphicsProfile.read(root, "fps_cap", "unset"), "unset"));
        d.graphics.put("Resolution marker", value(GraphicsProfile.read(root, "render_h", "native panel"),
                "native panel"));
        d.graphics.put("Anisotropy cap", value(GraphicsProfile.read(root, "aniso", "unset"), "unset"));
        d.graphics.put("Render scale", value(GraphicsProfile.read(root, "render_scale_pct", "100"), "100") + " %");
        d.graphics.put("Custom overrides", overrides(root));
        d.graphics.put("Display resolution", display(c));
        d.graphics.put("Render resolution", m.containsKey("render_w")
                ? m.get("render_w") + "x" + m.get("render_h") + " (logical)" : "unavailable");
        d.graphics.put("Window resolution", m.containsKey("window_w")
                ? m.get("window_w") + "x" + m.get("window_h") + " (swapchain)" : "unavailable");
        d.graphics.put("FPS (last second)", value(m.get("fps_now"), "unavailable"));
        d.graphics.put("FPS (average)", value(m.get("fps_avg"), "unavailable"));
        d.graphics.put("Frame time p50", value(m.get("frame_p50_ms"), "unavailable") + " ms");
        d.graphics.put("Frame time p95", value(m.get("frame_p95_ms"), "unavailable") + " ms");
        d.graphics.put("Frame time max", value(m.get("frame_max_ms"), "unavailable") + " ms");
        d.graphics.put("Frames presented", value(m.get("frames"), "unavailable"));
        d.graphics.put("Vulkan errors", value(m.get("vulkan_errors"), "0"));
        d.graphics.put("Vulkan init failures", m.containsKey("gpu_name")
                ? (Integer.parseInt(m.getOrDefault("vulkan_errors", "0")) > 0
                        ? m.get("vulkan_errors") + " Vulkan call(s) failed (see error history)"
                        : "none")
                : "renderer has not initialised since diagnostics started");
        d.graphics.put("Thermal status", thermal(c));
    }

    private static String overrides(File root) {
        StringBuilder sb = new StringBuilder();
        String[][] keys = { { "override_fps", "fps" }, { "override_res", "resolution" },
                { "override_aniso", "anisotropy" }, { "override_scale", "scale" } };
        for (String[] k : keys) {
            String v = GraphicsProfile.read(root, k[0], null);
            if (v != null) {
                if (sb.length() > 0) sb.append(", ");
                sb.append(k[1]).append('=').append(v);
            }
        }
        return sb.length() == 0 ? "none (preset defaults)" : sb.toString();
    }

    private static String display(Context c) {
        WindowManager wm = (WindowManager) c.getSystemService(Context.WINDOW_SERVICE);
        if (wm == null) return "unavailable";
        Display display = wm.getDefaultDisplay();
        DisplayMetrics metrics = new DisplayMetrics();
        display.getRealMetrics(metrics);
        return Math.max(metrics.widthPixels, metrics.heightPixels) + "x"
                + Math.min(metrics.widthPixels, metrics.heightPixels) + " @ "
                + Math.round(display.getRefreshRate()) + " Hz";
    }

    private static String thermal(Context c) {
        PowerManager pm = (PowerManager) c.getSystemService(Context.POWER_SERVICE);
        if (pm == null) return "unavailable";
        int status = pm.getCurrentThermalStatus();
        return GraphicsProfile.thermalName(status) + " (status " + status + ")";
    }

    /* ---- D ---------------------------------------------------------------- */

    private static void fillAudio(DiagData d, Map<String, String> m) {
        d.audio.put("Audio backend", "AAudio, low-latency shared mode, 16-bit PCM");
        d.audio.put("AAudio initialization", !m.containsKey("count_audio_open") ? "unavailable: no snapshot yet"
                : "1".equals(m.get("count_audio_open")) ? "output object open" : "no output object");
        d.audio.put("Stream state", !m.containsKey("count_audio_streaming") ? "unavailable"
                : "1".equals(m.get("count_audio_streaming")) ? "streaming"
                : "not streaming (retrying, or no device)");
        d.audio.put("Stream configuration", m.containsKey("count_audio_rate")
                ? m.get("count_audio_rate") + " Hz, " + m.getOrDefault("count_audio_channels", "?") + " ch"
                : "48000 Hz, 2 ch (requested)");
        d.audio.put("Paused by platform", value(m.get("count_audio_paused"), "0").equals("1") ? "yes" : "no");
        d.audio.put("Recovery attempts", value(m.get("count_audio_reopen_attempts"), "0"));
        d.audio.put("Failed reopen attempts", value(m.get("count_audio_reopen_failures_total"), "0"));
        d.audio.put("Consecutive reopen failures", value(m.get("count_audio_reopen_failures"), "0"));
        d.audio.put("Underruns (partial buffer)", value(m.get("count_audio_underruns"), "0"));
        d.audio.put("Starved buffers (nothing to play)", value(m.get("count_audio_starved"), "0"));
        d.audio.put("Stale audio blocks dropped on reopen", value(m.get("count_audio_stale_resets"), "0"));
        d.audio.put("Pause events", value(m.get("count_audio_pauses"), "0"));
        d.audio.put("Resume events", value(m.get("count_audio_resumes"), "0"));
        d.audio.put("Synchronization delays",
                "reopen backoff 250 ms for the first three failures, then 1000 ms; "
                        + "the mixer freezes its play clock while paused, so resume continues the stream");
    }

    /* ---- G ---------------------------------------------------------------- */

    private static String summarize(DiagData d, Map<String, String> m) {
        List<String> bits = new ArrayList<>();
        bits.add(("installed".equals(d.install.get("Installation status")) ? "game installed" : "game not installed"));
        if (m.containsKey("fps_now")) bits.add(m.get("fps_now") + " fps now");
        if (m.containsKey("frame_p95_ms")) bits.add("p95 " + m.get("frame_p95_ms") + " ms");
        bits.add("thermal " + d.graphics.get("Thermal status"));
        bits.add("audio " + d.audio.get("Stream state"));
        return String.join(", ", bits);
    }

    private static List<String> problems(Context c, File root, DiagData d, Map<String, String> m) {
        List<String> out = new ArrayList<>();
        if (!"installed".equals(d.install.get("Installation status")))
            out.add("The game data is not installed (install.ok is missing).");
        if (d.install.get("Required file verification").startsWith("FAILED"))
            out.add("The installed game data is incomplete: the Resource folder is missing.");
        if (!"none recorded".equals(d.install.get("Install errors")))
            out.add("The installer recorded errors; see the error history.");
        if (StorageAccess.destPath(c) != null && !StorageAccess.allFiles(c))
            out.add("A custom install folder is remembered but all files access is not granted; "
                    + "the runtime falls back to app storage.");
        if (m.isEmpty())
            out.add("No native metrics snapshot exists: the renderer has not run since the log started.");
        if (m.containsKey("gpu_name") && !"0".equals(m.getOrDefault("vulkan_errors", "0")))
            out.add("Vulkan reported " + m.get("vulkan_errors") + " failed call(s); see the error history.");
        if (m.containsKey("count_audio_open") && !"1".equals(m.getOrDefault("count_audio_streaming", "1")))
            out.add("The audio output is not streaming (a device change or a failed open).");
        String underruns = m.getOrDefault("count_audio_underruns", "0");
        String starved = m.getOrDefault("count_audio_starved", "0");
        if (!"0".equals(underruns) || !"0".equals(starved))
            out.add("Audio sink trouble: " + underruns + " underruns, " + starved + " starved buffers.");
        if (!"0".equals(m.getOrDefault("count_audio_reopen_failures_total", "0")))
            out.add("The audio output had " + m.get("count_audio_reopen_failures_total")
                    + " failed reopen attempts (recovered if the stream is streaming now).");
        String p95 = m.get("frame_p95_ms");
        if (p95 != null) {
            try {
                int cap = GraphicsProfile.readInt(root, "fps_cap", 0);
                if (cap > 0 && Integer.parseInt(p95) > (2000 / cap))
                    out.add("Frame pacing: p95 " + p95 + " ms is above the " + (1000 / cap)
                            + " ms budget for a " + cap + " fps cap.");
            } catch (NumberFormatException ignored) {
            }
        }
        if (!DiagLog.previousSession().isEmpty())
            out.add("The previous session did not shut down cleanly.");
        if (!d.crashLog.isEmpty()) out.add("A Java crash record exists from a previous run.");
        File nativeCrash = new File(DiagLog.root(), "native_crash.txt");
        if (nativeCrash.isFile() && DiagLog.readText(nativeCrash, 4000).contains("FATAL"))
            out.add("A native crash record exists (see the error history).");
        String last = DiagLog.lastError();
        if (!last.isEmpty()) {
            int nl = last.indexOf('\n');
            out.add("Last error: " + (nl > 0 ? last.substring(0, nl) : last));
        }
        if (out.isEmpty()) out.add("No problems detected.");
        return out;
    }

    /* ---- helpers ----------------------------------------------------------- */

    private static Map<String, String> readMetrics(File f) {
        Map<String, String> m = new LinkedHashMap<>();
        if (!f.isFile()) return m;
        try (InputStreamReader r = new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8)) {
            StringBuilder sb = new StringBuilder();
            char[] b = new char[1024];
            for (int n; (n = r.read(b)) > 0; ) sb.append(b, 0, n);
            for (String line : sb.toString().split("\n")) {
                int eq = line.indexOf('=');
                if (eq > 0) m.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
            }
        } catch (Exception e) {
            DiagLog.w("diagnostics", "could not read the native metrics: " + e.getMessage());
        }
        return m;
    }

    private static int[] count(File dir) {
        int[] out = new int[2];
        walk(dir, 0, out);
        return out;
    }

    private static void walk(File dir, int depth, int[] out) {
        if (depth > 16 || out[0] > 50000 || dir == null) return;
        File[] all = dir.listFiles();
        if (all == null) return;
        for (File f : all) {
            if (f.isDirectory()) {
                walk(f, depth + 1, out);
            } else if (f.isFile()) {
                out[0]++;
                out[1] += f.length();
            }
        }
    }

    private static String sha256(File f) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            try (FileInputStream in = new FileInputStream(f)) {
                byte[] b = new byte[1 << 16];
                for (int n; (n = in.read(b)) > 0; ) md.update(b, 0, n);
            }
            StringBuilder sb = new StringBuilder();
            for (byte x : md.digest()) sb.append(String.format("%02x", x));
            return sb.toString();
        } catch (Exception e) {
            return "unavailable (" + e.getMessage() + ")";
        }
    }

    private static String readSmall(File f) {
        if (!f.isFile() || f.length() > 8 * 1024) return null;
        return DiagLog.readText(f, 8192).trim();
    }

    private static String value(String v, String fallback) {
        return v == null || v.isEmpty() ? fallback : v;
    }
}
