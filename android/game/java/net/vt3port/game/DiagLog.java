package net.vt3port.game;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * The app's one logger. It owns a DiagCore over {@code <app files>/diagnostics}, receives the
 * native engine's entries through {@link #nativeWrite} (called from the JNI writer thread), and
 * keeps the small amount of state the Diagnostics screen shows.
 *
 * Initialised from the Application, so the native game process (relaunched by the system, without
 * the setup screen) still logs. Everything here is non-blocking except an explicit flush.
 */
public final class DiagLog {
    public static final String PREFS = "vt3_diag";
    public static final String DETAIL = "detail";
    public static final String LIMIT_KB = "log_limit_kb";
    public static final int DEFAULT_LIMIT_KB = 2048;
    public static final int KEEP = 3;

    private static DiagCore core;
    private static Context app;
    private static File root;
    private static long startedAt;
    private static String previousSession = "";
    private static boolean installedCrashHandler;

    private DiagLog() {}

    /** Safe to call more than once; the Application calls it first. */
    public static synchronized void init(Context c) {
        if (core != null) return;
        app = c.getApplicationContext();
        root = app.getExternalFilesDir(null);
        if (root == null) root = app.getFilesDir();
        startedAt = System.currentTimeMillis();
        File dir = new File(root, "diagnostics");
        SharedPreferences p = prefs();
        int kb = p.getInt(LIMIT_KB, DEFAULT_LIMIT_KB);
        core = new DiagCore(dir, "vt3", (long) kb * 1024L, KEEP);
        core.setLimit((long) kb * 1024L);
        boolean detail = p.getBoolean(DETAIL, false);
        writeDetailMarker(detail);
        String previous = core.beginSession();
        if (previous != null) {
            previousSession = "previous session did not shut down cleanly (started " + previous + ")";
            Log.println(Log.WARN, "vt3", previousSession);
        }
        if (!installedCrashHandler) {
            Thread.setDefaultUncaughtExceptionHandler(
                    new DiagCrashHandler(Thread.getDefaultUncaughtExceptionHandler()));
            installedCrashHandler = true;
        }
        i("app", "application start (pid " + android.os.Process.myPid() + ", API "
                + android.os.Build.VERSION.SDK_INT + ")");
        if (!previousSession.isEmpty()) w("app", previousSession);
    }

    public static synchronized void shutdown() {
        if (core == null) return;
        i("app", "application shutdown");
        core.endSession();
        core.close();
        core = null;
    }

    public static SharedPreferences prefs() {
        return app.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    public static DiagCore core() {
        return core;
    }

    public static File root() {
        return root;
    }

    public static File diagnosticsDir() {
        return core != null ? core.directory() : new File(root, "diagnostics");
    }

    public static long startedAt() {
        return startedAt;
    }

    public static String previousSession() {
        return previousSession;
    }

    public static boolean detail() {
        return app != null && prefs().getBoolean(DETAIL, false);
    }

    /** Detailed mode adds DEBUG entries and a marker the native runtime reads at its start. */
    public static synchronized void setDetail(boolean on) {
        if (app == null) return;
        prefs().edit().putBoolean(DETAIL, on).apply();
        writeDetailMarker(on);
        i("diagnostics", "detailed logging " + (on ? "enabled" : "disabled"));
    }

    public static void setLimitKb(int kb) {
        if (core == null || app == null) return;
        prefs().edit().putInt(LIMIT_KB, kb).apply();
        core.setLimit((long) kb * 1024L);
        i("diagnostics", "log size limit set to " + kb + " KB");
    }

    private static void writeDetailMarker(boolean on) {
        if (root == null) return;
        File marker = new File(root, "diag_detail");
        try {
            if (on) {
                try (Writer w = new OutputStreamWriter(new FileOutputStream(marker), StandardCharsets.UTF_8)) {
                    w.write("1");
                }
            } else {
                marker.delete();
            }
        } catch (Exception e) {
            Log.w("vt3", "could not write the detail marker: " + e.getMessage());
        }
    }

    /* ---- logging ------------------------------------------------------------ */

    public static void d(String component, String message) {
        if (!detail()) return;
        write(DiagCore.DEBUG, component, 0, message, null);
    }

    public static void i(String component, String message) {
        write(DiagCore.INFO, component, 0, message, null);
    }

    public static void w(String component, String message) {
        write(DiagCore.WARN, component, 0, message, null);
    }

    public static void e(String component, String message) {
        write(DiagCore.ERROR, component, 0, message, null);
    }

    public static void e(String component, int code, String message, Throwable t) {
        write(DiagCore.ERROR, component, code, message, t);
    }

    public static void f(String component, int code, String message, Throwable t) {
        write(DiagCore.FATAL, component, code, message, t);
    }

    private static void write(int level, String component, int code, String message, Throwable t) {
        int prio = level == DiagCore.FATAL ? Log.ASSERT : level == DiagCore.ERROR ? Log.ERROR
                : level == DiagCore.WARN ? Log.WARN : level == DiagCore.DEBUG ? Log.DEBUG : Log.INFO;
        Log.println(prio, "vt3", (component == null ? "app" : component) + ": " + message);
        DiagCore c = core;
        if (c == null) return;
        c.log(level, component, code, message, t);
    }

    /**
     * Called by the native writer thread through JNI. Levels match vt3_log_level: 0 DEBUG,
     * 1 INFO, 2 WARN, 3 ERROR, 4 FATAL. If the app has not initialised yet the entry is already
     * in logcat (the native sink writes there too), so nothing is lost silently.
     */
    @SuppressWarnings("unused")   // called from native code
    private static void nativeWrite(int level, String component, int code, String message) {
        DiagCore c = core;
        if (c == null) return;
        int lv = Math.max(0, Math.min(4, level));
        if (lv == DiagCore.DEBUG && !detail()) return;
        c.log(lv, component, code, message);
    }

    public static void flush(long timeoutMs) {
        DiagCore c = core;
        if (c != null) c.flush(timeoutMs);
    }

    /* ---- state for the screen ----------------------------------------------- */

    public static long sizeBytes() {
        return core != null ? core.sizeBytes() : 0;
    }

    public static String lastError() {
        return core != null ? core.lastError() : "";
    }

    public static String lastReportName() {
        return app == null ? "" : prefs().getString("last_report", "");
    }

    public static void setLastReportName(String name) {
        if (app != null) prefs().edit().putString("last_report", name).apply();
    }

    public static long lastReportAt() {
        return app == null ? 0 : prefs().getLong("last_report_at", 0);
    }

    public static void setLastReportAt(long at) {
        if (app != null) prefs().edit().putLong("last_report_at", at).apply();
    }

    public static synchronized void clear() {
        if (core == null) return;
        core.clear();
        previousSession = "";
        i("diagnostics", "logs cleared");
    }

    /** A crash record written before the process dies; the log queue may not have drained. */
    static void recordCrash(Thread thread, Throwable error) {
        File dir = diagnosticsDir();
        if (!dir.isDirectory()) dir.mkdirs();
        File f = new File(dir, "crash.txt");
        File tmp = new File(dir, "crash.txt.tmp");
        String stamp = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date());
        try (Writer w = new OutputStreamWriter(new FileOutputStream(tmp), StandardCharsets.UTF_8)) {
            w.write("FATAL: uncaught exception at " + stamp + '\n');
            w.write("thread: " + (thread == null ? "?" : thread.getName()) + '\n');
            if (error != null) {
                w.write(error.toString() + '\n');
                for (StackTraceElement el : error.getStackTrace()) w.write("at " + el + '\n');
                Throwable cause = error.getCause();
                if (cause != null && cause != error) {
                    w.write("caused by " + cause + '\n');
                    for (StackTraceElement el : cause.getStackTrace()) w.write("at " + el + '\n');
                }
            }
            w.flush();
        } catch (Exception ignored) {
            return;
        }
        if (!tmp.renameTo(f)) tmp.delete();
    }

    static String readText(File f, int maxBytes) {
        if (f == null || !f.isFile()) return "";
        try (InputStreamReader r = new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8)) {
            StringBuilder sb = new StringBuilder();
            char[] b = new char[1024];
            for (int n; (n = r.read(b)) > 0; ) {
                sb.append(b, 0, n);
                if (sb.length() > maxBytes) {
                    sb.delete(0, sb.length() - maxBytes);
                }
            }
            return sb.toString();
        } catch (Exception e) {
            return "";
        }
    }
}
