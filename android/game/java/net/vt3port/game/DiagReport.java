package net.vt3port.game;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Builds the diagnostic report as plain text. No android.* here: the host test feeds it a filled
 * DiagData and a real DiagCore and checks the file it produces.
 *
 * The report is deliberately verbose about what was measured and explicit about what was not: an
 * unavailable value is written as "unavailable", never filled in with a guess. Warnings and errors
 * are separated from informational lines so a support reader can see the difference.
 */
public final class DiagReport {
    public static final String TITLE = "VT3 Android diagnostic report";
    public static final String FORMAT = "1";
    /** The report's name, for the app's own copy and for the Save/Share sheet. One file, rewritten
     *  in place; the date it was generated is written inside the report, not appended to the name. */
    public static final String FILE_NAME = "VT3_Diagnostic_Report.txt";

    private DiagReport() {}

    public static String suggestName() {
        return FILE_NAME;
    }

    public static String build(DiagData d, DiagCore core) {
        StringBuilder sb = new StringBuilder(16 * 1024);
        SimpleDateFormat full = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US);
        sb.append(TITLE).append('\n');
        sb.append("Generated: ").append(full.format(new Date(d.generatedAt))).append('\n');
        sb.append("Report format: ").append(FORMAT).append('\n');
        sb.append("Plain text, UTF-8. Written by the app itself; it is the complete report.\n\n");

        section(sb, "A. Application information", d.app);
        section(sb, "B. Game installation information", d.install);
        section(sb, "C. Graphics and performance", d.graphics);
        section(sb, "D. Audio diagnostics", d.audio);

        sb.append("E. Android lifecycle\n");
        sb.append("   (oldest first; startup, window, pause/resume and storage events)\n");
        if (d.lifecycle.isEmpty()) {
            sb.append("   No lifecycle events recorded.\n");
        } else {
            for (String line : d.lifecycle) sb.append("   ").append(line).append('\n');
        }
        sb.append('\n');

        sb.append("F. Error history\n");
        sb.append("   Severity prefixes are the logger's: WARN is recoverable, ERROR is a failed\n");
        sb.append("   operation, FATAL is a session-ending fault. INFO lines are not errors.\n");
        int warnings = 0, errors = 0;
        for (String line : d.errors) {
            if (line.contains(" WARN ")) warnings++;
            else errors++;
        }
        sb.append("   Warnings recorded: ").append(warnings)
          .append("; errors recorded: ").append(errors).append('\n');
        if (d.errors.isEmpty()) {
            sb.append("   No warnings or errors were recorded in the retained log.\n");
        } else {
            for (String line : d.errors) sb.append("   ").append(line).append('\n');
        }
        if (d.lastError != null && !d.lastError.isEmpty()) {
            sb.append("\n   Last error (kept across restarts):\n");
            for (String line : d.lastError.split("\n")) sb.append("     ").append(line).append('\n');
        }
        sb.append('\n');
        if (d.crashLog != null && !d.crashLog.isEmpty()) {
            sb.append("   Native crash record (written by the signal handler; this cannot catch a\n");
            sb.append("   process that is killed or a crash before the handler runs):\n");
            for (String line : d.crashLog.split("\n")) sb.append("     ").append(line).append('\n');
            sb.append('\n');
        }
        if (d.previousSession != null && !d.previousSession.isEmpty()) {
            sb.append("   Previous session: ").append(d.previousSession).append('\n');
        }

        sb.append("G. Diagnostic summary\n");
        if (d.problems.isEmpty()) {
            sb.append("   No problems were detected from the available data.\n");
        } else {
            sb.append("   Detected problems:\n");
            for (String p : d.problems) sb.append("   - ").append(p).append('\n');
        }
        sb.append("   Last known state: ")
          .append(value(d.app.get("Last known state"), "unavailable")).append('\n');
        sb.append("   Log size at generation: ").append(d.logBytes).append(" bytes");
        if (d.droppedEntries > 0) {
            sb.append("; ").append(d.droppedEntries)
              .append(" entries were dropped because the log queue was full (frames were never\n");
            sb.append("   blocked by this: the drop is the logger's backpressure policy)");
        }
        sb.append(".\n");
        sb.append('\n');
        if (core != null) {
            List<String> tail = core.errors(6000);
            sb.append("   Log lines at WARN or above in the retained tail: ").append(tail.size()).append('\n');
        }
        sb.append("\n== End of report ==\n");
        return sb.toString();
    }

    private static void section(StringBuilder sb, String title, Map<String, String> values) {
        sb.append(title).append('\n');
        if (values.isEmpty()) {
            sb.append("   No data collected.\n");
        } else {
            for (Map.Entry<String, String> e : values.entrySet()) {
                sb.append("   ").append(pad(e.getKey(), 26)).append(": ")
                  .append(value(e.getValue(), "unavailable")).append('\n');
            }
        }
        sb.append('\n');
    }

    private static String pad(String s, int width) {
        StringBuilder sb = new StringBuilder(s == null ? "" : s);
        while (sb.length() < width) sb.append(' ');
        return sb.toString();
    }

    private static String value(String v, String fallback) {
        return v == null || v.isEmpty() ? fallback : v;
    }
}
