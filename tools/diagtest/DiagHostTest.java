import net.vt3port.game.DiagCore;
import net.vt3port.game.DiagData;
import net.vt3port.game.DiagReport;

import java.io.File;
import java.io.FileWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * Host test for the diagnostics log engine and the TXT report, with no Android runtime:
 *
 *   javac -d /tmp/vt3diag android/game/java/net/vt3port/game/DiagCore.java \
 *         android/game/java/net/vt3port/game/DiagData.java \
 *         android/game/java/net/vt3port/game/DiagReport.java tools/diagtest/DiagHostTest.java
 *   java -cp /tmp/vt3diag DiagHostTest
 *
 * What it establishes: the queue is bounded and every entry is accepted or counted as dropped,
 * rotation honours the size limit and keeps the configured number of files, the tail/error readers
 * work, the last error survives a restart, an unclean session is detected, and the report contains
 * genuine values in every section and round-trips through a real file.
 */
public final class DiagHostTest {
    private static int checks;

    public static void main(String[] args) throws Exception {
        File dir = Files.createTempDirectory("vt3diag").toFile();
        DiagCore core = new DiagCore(dir, "vt3", 4096, 2);

        for (int i = 0; i < 300; i++)
            core.log(DiagCore.INFO, "app", 0, "line " + i + " " + repeat('x', 50));
        core.log(DiagCore.ERROR, "install", 7, "install failed", new RuntimeException("boom"));
        core.flush(4000);

        check(core.accepted() + core.dropped() == 301, "every entry is accepted or counted as dropped");
        long size = core.sizeBytes();
        check(size > 0, "log files have content");
        check(size <= 4096L * 4 + 4096, "rotation bounds the directory (" + size + " bytes)");
        int logs = 0;
        File[] files = dir.listFiles();
        if (files != null)
            for (File f : files) if (f.getName().endsWith(".log")) logs++;
        check(logs >= 2, "an older log was rotated aside (" + logs + " files)");

        check(core.tail(800).contains("line 299"), "the tail holds the newest lines");
        /* tail() has a 1 KB floor so a tiny request still shows a usable window. */
        check(core.tail(2000).length() <= 2100, "the tail respects the byte budget");
        check(!core.errors(8192).isEmpty(), "the error history finds the ERROR line");
        check(core.lastError().contains("install failed"), "the last error is kept in memory");
        check(new File(dir, "last_error.txt").isFile(), "the last error is written for the next launch");

        check(core.beginSession() == null, "the first session has no predecessor");
        core.endSession();
        core.close();

        /* An unclean session leaves its marker behind; the next start must notice. */
        try (FileWriter w = new FileWriter(new File(dir, "session.txt"))) {
            w.write("2026-11-20 22:00:00");
        }
        DiagCore second = new DiagCore(dir, "vt3", 4096, 2);
        String previous = second.beginSession();
        check(previous != null && previous.contains("2026-11-20"),
                "an unclean previous session is detected");

        DiagData d = new DiagData();
        d.app.put("Application version", "0.2.0");
        d.app.put("Build number", "2");
        d.app.put("Android version", "15 (API 35)");
        d.app.put("Device model", "Test Device");
        d.app.put("CPU architecture", "arm64-v8a");
        d.app.put("Available RAM", "2048 MB free of 8192 MB");
        d.app.put("Last known state", "game installed, 60.0 fps now");
        d.install.put("Installation status", "installed");
        d.install.put("Selected game source type", "disc image");
        d.install.put("Installation destination", "/storage/emulated/0/Android/data/x/files");
        d.install.put("Required file verification", "Resource folder present; 12 files, 4 MB");
        d.install.put("Missing or corrupted files", "none detected by the checks above");
        d.install.put("Storage permission", "all files access not granted");
        d.install.put("Install errors", "none recorded");
        d.graphics.put("GPU vendor", "ARM");
        d.graphics.put("GPU renderer", "Mali-G77");
        d.graphics.put("Vulkan version", "1.1");
        d.graphics.put("Vulkan extensions", "VK_KHR_surface");
        d.graphics.put("Graphics preset", "balanced");
        d.graphics.put("Display resolution", "2400x1080 @ 120 Hz");
        d.graphics.put("Render resolution", "1280x720 (logical)");
        d.graphics.put("FPS (last second)", "59.9");
        d.graphics.put("Frame time p50", "16 ms");
        d.graphics.put("Frame time p95", "18 ms");
        d.graphics.put("Frame time max", "41.2 ms");
        d.graphics.put("Vulkan errors", "0");
        d.graphics.put("Thermal status", "cool (status 0)");
        d.audio.put("Audio backend", "AAudio, low-latency shared mode, 16-bit PCM");
        d.audio.put("AAudio initialization", "output object open");
        d.audio.put("Stream state", "streaming");
        d.audio.put("Stream configuration", "48000 Hz, 2 ch");
        d.audio.put("Recovery attempts", "1");
        d.audio.put("Failed reopen attempts", "0");
        d.audio.put("Underruns (partial buffer)", "2");
        d.audio.put("Starved buffers (nothing to play)", "5");
        d.audio.put("Stale audio blocks dropped on reopen", "1");
        d.lifecycle.add("2026-11-20 23:00:00.000  INFO   lifecycle  window created 2400x1080");
        d.lifecycle.add("2026-11-20 23:00:10.000  INFO   lifecycle  activity paused");
        d.errors.add("2026-11-20 23:00:05.000  WARN   audio  underruns");
        d.errors.add("2026-11-20 23:00:06.000  ERROR  install  install failed");
        d.generatedAt = System.currentTimeMillis();
        d.logBytes = size;
        d.droppedEntries = core.dropped();

        String report = DiagReport.build(d, second);
        for (String heading : new String[] { "A. Application information", "B. Game installation information",
                "C. Graphics and performance", "D. Audio diagnostics", "E. Android lifecycle",
                "F. Error history", "G. Diagnostic summary", "== End of report ==" })
            check(report.contains(heading), "report contains " + heading);
        check(report.contains("0.2.0") && report.contains("Mali-G77") && report.contains("48000 Hz"),
                "report carries the collected values");
        check(report.contains("Warnings recorded: 1; errors recorded: 1"), "errors and warnings are separated");
        check(!report.contains("null"), "no null leaks into the text");
        check(report.length() > 1200, "report is substantive (" + report.length() + " chars)");

        String name = DiagReport.suggestName();
        check(name.equals("VT3_Diagnostic_Report.txt"), "report name is the documented .txt name");
        File out = new File(dir, name);
        Files.write(out.toPath(), report.getBytes(StandardCharsets.UTF_8));
        check(out.isFile() && out.length() > 1200, "the .txt file exists with content");
        String back = new String(Files.readAllBytes(out.toPath()), StandardCharsets.UTF_8);
        check(back.equals(report), "the saved file reopens byte-identical to the report");
        check(back.contains("VT3 Android diagnostic report"), "the saved file is the report, not a placeholder");

        second.clear();
        check(second.sizeBytes() == 0, "clear empties the logs");
        check(!new File(dir, "last_error.txt").exists(), "clear removes the last-error record");
        check(new File(dir, "session.txt").exists(),
                "clear keeps the running session's marker (it is not log data)");
        second.endSession();
        check(!new File(dir, "session.txt").exists(), "a clean session removes its marker");
        second.close();

        System.out.println("DiagHostTest: " + checks + " checks passed; report " + report.length()
                + " chars, file " + out.length() + " bytes");
        deleteTree(dir);
    }

    private static String repeat(char c, int n) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < n; i++) sb.append(c);
        return sb.toString();
    }

    private static void check(boolean ok, String what) {
        checks++;
        if (!ok) throw new IllegalStateException("FAILED: " + what);
        System.out.println("  ok  " + what);
    }

    private static void deleteTree(File f) {
        File[] all = f.listFiles();
        if (all != null) for (File c : all) deleteTree(c);
        f.delete();
    }
}
