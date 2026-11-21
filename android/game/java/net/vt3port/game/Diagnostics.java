package net.vt3port.game;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.text.method.ScrollingMovementMethod;
import android.util.TypedValue;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Settings -> Diagnostics. Every control here does the thing it says:
 *
 *   View logs          the retained log tail, on screen.
 *   Generate report    builds the TXT report from live data and shows it.
 *   Save report        the Storage Access Framework's create-document picker; the file written is
 *                      reopened and checked before the screen says it was saved.
 *   Share report       a real copy in the app's cache, shared through a content provider.
 *   Open last report   the document created by Save, opened by whatever reads text.
 *   Clear logs         deletes the log files, the last-error record and crash records only.
 *   View last error    the last warning/error of this or the previous session.
 *
 * No button is decorative and nothing claims success without checking.
 */
public class Diagnostics extends Activity {
    static final int SAVE = 101;
    static final String LAST_URI = "last_report_uri";

    TextView state, status;
    CheckBox detail;
    String pendingText;
    String pendingName;

    @Override
    protected void onCreate(Bundle bundle) {
        super.onCreate(bundle);
        DiagLog.init(this);
        DiagLog.i("diagnostics", "screen opened");

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(getColor(R.color.vt3_page));
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(20), dp(20), dp(20), dp(24));
        scroll.addView(box);

        TextView title = new TextView(this);
        title.setText(R.string.diagnostics_title);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        title.setTextColor(getColor(R.color.vt3_text));
        box.addView(title);

        TextView hint = new TextView(this);
        hint.setText(R.string.diagnostics_hint);
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        hint.setTextColor(getColor(R.color.vt3_muted));
        hint.setPadding(0, dp(8), 0, dp(16));
        box.addView(hint);

        state = new TextView(this);
        state.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        state.setTextColor(getColor(R.color.vt3_text));
        state.setBackgroundResource(R.drawable.bg_card);
        state.setPadding(dp(12), dp(12), dp(12), dp(12));
        box.addView(state);

        detail = new CheckBox(this);
        detail.setText(R.string.diagnostics_detail);
        detail.setTextColor(getColor(R.color.vt3_text));
        detail.setChecked(DiagLog.detail());
        detail.setPadding(0, dp(12), 0, dp(4));
        detail.setOnCheckedChangeListener((v, checked) -> {
            DiagLog.setDetail(checked);
            DiagLog.i("diagnostics", "detailed mode " + (checked ? "on" : "off"));
            refresh();
        });
        box.addView(detail);

        button(box, R.string.diagnostics_view, this::viewLogs);
        button(box, R.string.diagnostics_generate, this::generate);
        button(box, R.string.diagnostics_save, this::save);
        button(box, R.string.diagnostics_share, this::share);
        button(box, R.string.diagnostics_open, this::openLast);
        button(box, R.string.diagnostics_clear, this::clear);
        button(box, R.string.diagnostics_last_error, this::viewLastError);

        status = new TextView(this);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        status.setTextColor(getColor(R.color.vt3_muted));
        status.setPadding(0, dp(16), 0, 0);
        box.addView(status);

        TextView note = new TextView(this);
        note.setText(R.string.diagnostics_note);
        note.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        note.setTextColor(getColor(R.color.vt3_muted));
        note.setPadding(0, dp(16), 0, 0);
        box.addView(note);

        setContentView(scroll);
        refresh();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private void button(LinearLayout box, int label, Runnable action) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        box.addView(b, lp);
        b.setOnClickListener(v -> action.run());
    }

    private int dp(int v) {
        return Math.round(getResources().getDisplayMetrics().density * v);
    }

    private void refresh() {
        String version = "unknown";
        try {
            version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception ignored) {
        }
        SimpleDateFormat fmt = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US);
        long at = DiagLog.lastReportAt();
        String last = DiagLog.lastError();
        int nl = last.indexOf('\n');
        state.setText(getString(R.string.diagnostics_state,
                version,
                humanBytes(DiagLog.sizeBytes()),
                DiagLog.core() != null ? Long.toString(DiagLog.core().accepted()) : "0",
                DiagLog.core() != null ? Long.toString(DiagLog.core().dropped()) : "0",
                at == 0 ? getString(R.string.diagnostics_never) : fmt.format(new Date(at)),
                last.isEmpty() ? getString(R.string.diagnostics_no_error)
                        : (nl > 0 ? last.substring(0, nl) : last)));
    }

    private static String humanBytes(long n) {
        if (n < 1024) return n + " B";
        if (n < 1024 * 1024) return (n / 1024) + " KB";
        return String.format(Locale.US, "%.1f MB", n / 1048576.0);
    }

    /* ---- actions ------------------------------------------------------------ */

    private void viewLogs() {
        DiagCore core = DiagLog.core();
        if (core == null) {
            say(getString(R.string.diagnostics_no_log));
            return;
        }
        String text = core.tail(20000);
        if (text.trim().isEmpty()) text = getString(R.string.diagnostics_empty_log);
        DiagLog.i("diagnostics", "logs viewed");
        dialog(getString(R.string.diagnostics_view), text, true);
    }

    /** Builds the report from live data and keeps it for Save/Share. */
    private String buildReport() {
        DiagCore core = DiagLog.core();
        DiagData data = DiagInfo.collect(this, core);
        String text = DiagReport.build(data, core);
        pendingText = text;
        pendingName = DiagReport.suggestName();
        DiagLog.i("diagnostics", "report generated, " + text.length() + " chars, " + pendingName);
        return text;
    }

    private void generate() {
        String text = buildReport();
        String preview = text.length() > 12000 ? text.substring(0, 12000) + "\n[...]" : text;
        TextView view = new TextView(this);
        view.setText(preview);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        view.setTypeface(android.graphics.Typeface.MONOSPACE);
        view.setTextColor(getColor(R.color.vt3_text));
        view.setPadding(dp(12), dp(8), dp(12), dp(8));
        view.setTextIsSelectable(true);
        ScrollView s = new ScrollView(this);
        s.addView(view);
        new AlertDialog.Builder(this)
                .setTitle(R.string.diagnostics_generate)
                .setView(s)
                .setPositiveButton(R.string.diagnostics_save, (d, w) -> save())
                .setNegativeButton(android.R.string.cancel, null)
                .show();
        say(getString(R.string.diagnostics_generated, pendingName, text.length()));
    }

    private void save() {
        if (pendingText == null) buildReport();   /* generating first is what the button means */
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("text/plain")
                .putExtra(Intent.EXTRA_TITLE, pendingName);
        try {
            startActivityForResult(i, SAVE);
        } catch (ActivityNotFoundException e) {
            DiagLog.e("diagnostics", 0, "no document provider for saving", e);
            say(getString(R.string.diagnostics_no_picker));
        }
    }

    private void share() {
        String text = buildReport();
        try {
            File f = DiagFileProvider.write(this, pendingName, text);
            Uri uri = DiagFileProvider.uriFor(this, f.getName());
            Intent send = new Intent(Intent.ACTION_SEND)
                    .setType("text/plain")
                    .putExtra(Intent.EXTRA_SUBJECT, getString(R.string.diagnostics_title))
                    .putExtra(Intent.EXTRA_STREAM, uri)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(send, getString(R.string.diagnostics_share)));
            say(getString(R.string.diagnostics_shared, f.length()));
        } catch (Exception e) {
            DiagLog.e("diagnostics", 0, "share failed", e);
            say(getString(R.string.diagnostics_share_failed, String.valueOf(e.getMessage())));
        }
    }

    private void openLast() {
        String s = DiagLog.prefs().getString(LAST_URI, null);
        if (s == null) {
            say(getString(R.string.diagnostics_nothing_saved));
            return;
        }
        Uri uri = Uri.parse(s);
        try {
            startActivity(new Intent(Intent.ACTION_VIEW).setDataAndType(uri, "text/plain")
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION));
            DiagLog.i("diagnostics", "opening last report " + s);
        } catch (ActivityNotFoundException e) {
            DiagLog.w("diagnostics", "no viewer for text files: " + e.getMessage());
            say(getString(R.string.diagnostics_no_viewer));
        }
    }

    private void clear() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.diagnostics_clear)
                .setMessage(R.string.diagnostics_clear_ask)
                .setPositiveButton(R.string.diagnostics_clear, (d, w) -> {
                    DiagLog.clear();
                    pendingText = null;
                    pendingName = null;
                    refresh();
                    say(getString(R.string.diagnostics_cleared));
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void viewLastError() {
        String last = DiagLog.lastError();
        String crash = DiagLog.readText(new File(DiagLog.diagnosticsDir(), "crash.txt"), 4000);
        String nativeCrash = DiagLog.readText(new File(DiagLog.root(), "native_crash.txt"), 4000);
        StringBuilder sb = new StringBuilder();
        if (last.isEmpty()) sb.append(getString(R.string.diagnostics_no_error));
        else sb.append(last);
        if (!crash.isEmpty()) sb.append("\n\nJava crash record:\n").append(crash);
        if (!nativeCrash.isEmpty()) sb.append("\n\nNative crash record:\n").append(nativeCrash);
        dialog(getString(R.string.diagnostics_last_error), sb.toString(), true);
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (code != SAVE) return;
        if (result != RESULT_OK || data == null || data.getData() == null) {
            say(getString(R.string.diagnostics_save_cancelled));
            return;
        }
        Uri uri = data.getData();
        String text = pendingText != null ? pendingText : buildReport();
        try {
            try (OutputStream out = getContentResolver().openOutputStream(uri, "wt")) {
                if (out == null) throw new java.io.IOException("the provider returned no stream");
                out.write(text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
                out.flush();
            }
            /* Verify it is really there and readable, not just that the write call returned. */
            int bytes = 0;
            boolean looksRight = false;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null) throw new java.io.IOException("the report cannot be reopened");
                byte[] b = new byte[4096];
                StringBuilder head = new StringBuilder();
                for (int n; (n = in.read(b)) > 0; ) {
                    bytes += n;
                    if (head.length() < 200) head.append(new String(b, 0, n, java.nio.charset.StandardCharsets.UTF_8));
                }
                looksRight = head.toString().contains("VT3 Android diagnostic report");
            }
            if (bytes < 200 || !looksRight)
                throw new java.io.IOException("the saved file is empty or not a report (" + bytes + " bytes)");
            try {
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (Exception ignored) {
                /* not every provider grants persistence; opening now still works */
            }
            DiagLog.prefs().edit().putString(LAST_URI, uri.toString()).apply();
            DiagLog.setLastReportName(pendingName);
            DiagLog.setLastReportAt(System.currentTimeMillis());
            DiagLog.i("diagnostics", "report saved and verified: " + uri + ", " + bytes + " bytes");
            say(getString(R.string.diagnostics_saved, bytes));
            refresh();
        } catch (Exception e) {
            DiagLog.e("diagnostics", 0, "saving the report failed", e);
            say(getString(R.string.diagnostics_save_failed, String.valueOf(e.getMessage())));
        }
    }

    /* ---- UI helpers --------------------------------------------------------- */

    private void dialog(String title, String text, boolean scroll) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        view.setTypeface(android.graphics.Typeface.MONOSPACE);
        view.setTextColor(getColor(R.color.vt3_text));
        view.setPadding(dp(12), dp(8), dp(12), dp(8));
        view.setTextIsSelectable(true);
        if (scroll) {
            view.setMovementMethod(new ScrollingMovementMethod());
            ScrollView s = new ScrollView(this);
            s.addView(view);
            new AlertDialog.Builder(this).setTitle(title).setView(s)
                    .setPositiveButton(android.R.string.ok, null).show();
        } else {
            new AlertDialog.Builder(this).setTitle(title).setMessage(text)
                    .setPositiveButton(android.R.string.ok, null).show();
        }
    }

    private void say(String s) {
        status.setText(s);
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show();
    }
}
