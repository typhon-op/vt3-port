package net.vt3port.game;

import android.content.Context;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * The on-screen controls' settings, as the runtime reads them.
 *
 * The file is touch.cfg in the game's data root: one key=value per line, read by
 * runtime/src/ui/touch_config.c. This class is the editor's view of the same file, with the same
 * key names and the same defaults, so a value changed here is one the runtime acts on.
 *
 * Two copies of the defaults exist (here and in touch_config.c) because the two sides share no
 * code; tools/touchtest pins the runtime's copy, and this one is written out on first use, so the
 * file on disk is the single source both read afterwards.
 *
 * A save never overwrites the last good file: it writes touch.cfg.tmp, keeps the previous file as
 * touch.cfg.bak, then renames the new one into place. A failed write leaves both untouched.
 */
final class TouchConfig {
    static final String FILE = "touch.cfg";
    static final String BACKUP = "touch.cfg.bak";
    static final int VERSION = 1;

    static final String[] LAYOUT_IDS = { "standard", "compact", "swipe", "custom1", "custom2" };
    static final String[] LAYOUT_NAMES = { "Standard", "Compact", "Swipe", "Custom 1", "Custom 2" };
    static final String[] PROFILE_IDS = { "default", "classic", "floating", "dynamic", "custom1", "custom2" };
    static final String[] PROFILE_NAMES = { "Default", "Classic", "Floating", "Dynamic", "Custom 1", "Custom 2" };
    static final String[] CTL_IDS = { "stick", "a", "b", "x", "y", "lb", "rb", "select", "start", "swipe", "menu" };
    static final String[] CTL_NAMES = { "Left stick", "A", "B", "X", "Y", "LB", "RB", "Select", "Start", "One-Button Swipe", "Menu button" };
    static final String[] DIRS = { "up", "down", "left", "right" };

    final LinkedHashMap<String, String> v = new LinkedHashMap<>();
    /** Set when the file on disk was written by a newer app: editing is refused, not overwritten. */
    String problem;

    static File file(Context c) { return new File(Game.root(c), FILE); }
    static File backupFile(Context c) { return new File(Game.root(c), BACKUP); }

    static TouchConfig defaults() {
        TouchConfig t = new TouchConfig();
        t.v.put("version", Integer.toString(VERSION));
        t.v.put("layout", "standard");
        t.v.put("profile", "default");
        t.v.put("lock", "1");
        t.v.put("swipe.enabled", "1");
        t.v.put("swipe.labels", "1");
        t.v.put("swipe.charge", "0");
        t.v.put("swipe.min_dist", "0.180");
        t.v.put("swipe.map.up", "a");
        t.v.put("swipe.map.down", "b");
        t.v.put("swipe.map.left", "x");
        t.v.put("swipe.map.right", "y");
        for (String l : LAYOUT_IDS)
            for (String k : CTL_IDS) {
                t.v.put("l." + l + "." + k + ".x", "-1.0000");
                t.v.put("l." + l + "." + k + ".y", "-1.0000");
                t.v.put("l." + l + "." + k + ".size", "1.000");
                t.v.put("l." + l + "." + k + ".opacity", "1.000");
                t.v.put("l." + l + "." + k + ".visible", "1");
                t.v.put("l." + l + "." + k + ".removed", "0");
            }
        // The swipe pad is not part of the two layouts that keep the original buttons.
        for (String l : LAYOUT_IDS) t.v.put("l." + l + ".swipe.removed", "1");
        t.v.put("l.swipe.swipe.removed", "0");
        // Preset geometry, mirrored from touch_config.c.
        t.setCtl("compact", "stick", 0.155f, 0.800f, 0.85f);
        t.setCtl("compact", "a", 0.855f, 0.875f, 0.85f);
        t.setCtl("compact", "b", 0.945f, 0.755f, 0.85f);
        t.setCtl("compact", "x", 0.765f, 0.755f, 0.85f);
        t.setCtl("compact", "y", 0.855f, 0.635f, 0.85f);
        t.setCtl("compact", "lb", 0.135f, 0.055f, 0.85f);
        t.setCtl("compact", "rb", 0.865f, 0.055f, 0.85f);
        t.setCtl("compact", "select", 0.420f, 0.945f, 0.90f);
        t.setCtl("compact", "start", 0.580f, 0.945f, 0.90f);
        t.setCtl("swipe", "stick", 0.170f, 0.820f, 0.90f);
        t.setCtl("swipe", "swipe", 0.720f, 0.740f, 1.50f);
        for (String k : new String[] { "a", "b", "x", "y" })
            t.v.put("l.swipe." + k + ".removed", "1");
        for (String p : PROFILE_IDS) {
            t.v.put("p." + p + ".deadzone", "0.140");
            t.v.put("p." + p + ".sensitivity", "1.000");
            t.v.put("p." + p + ".range", "1.000");
            t.v.put("p." + p + ".progressive", "0");
            t.v.put("p." + p + ".floating", "0");
            t.v.put("p." + p + ".smooth", "0");
            t.v.put("p." + p + ".invertx", "0");
            t.v.put("p." + p + ".inverty", "0");
            t.v.put("p." + p + ".sensx", "1.000");
            t.v.put("p." + p + ".sensy", "1.000");
        }
        t.v.put("p.classic.deadzone", "0.100");
        t.v.put("p.floating.floating", "1");
        t.v.put("p.dynamic.progressive", "1");
        return t;
    }

    /** Reads the file, or the shipped defaults when there is none. Never throws. */
    static TouchConfig load(Context c) {
        File f = file(c);
        if (!f.isFile()) {
            TouchConfig t = defaults();
            t.save(c);
            return t;
        }
        TouchConfig t = defaults();
        try (FileInputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            byte[] b = new byte[4096];
            for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n);
            String text = o.toString("UTF-8");
            for (String line : text.split("\n")) {
                String s = line.trim();
                if (s.isEmpty() || s.startsWith("#")) continue;
                int eq = s.indexOf('=');
                if (eq <= 0) continue;
                t.v.put(s.substring(0, eq).trim(), s.substring(eq + 1).trim());
            }
            int ver = t.i("version", 0);
            if (ver > VERSION) t.problem = "touch.cfg was written by a newer version of the app";
        } catch (IOException | RuntimeException e) {
            DiagLog.w("touch", "could not read touch.cfg: " + e.getMessage());
            t.problem = "touch.cfg could not be read; the shipped layout is in use";
        }
        return t;
    }

    /**
     * Writes through a temporary file, keeping the previous file as touch.cfg.bak. Returns false and
     * changes nothing on disk when the write fails.
     */
    boolean save(Context c) {
        if (problem != null) {
            DiagLog.w("touch", "not saving: " + problem);
            return false;
        }
        File f = file(c), bak = backupFile(c), tmp = new File(f.getPath() + ".tmp");
        StringBuilder s = new StringBuilder();
        for (Map.Entry<String, String> e : v.entrySet()) s.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        try {
            try (FileOutputStream out = new FileOutputStream(tmp)) {
                out.write(s.toString().getBytes(StandardCharsets.UTF_8));
                out.getFD().sync();
            }
            if (f.isFile()) {
                // Keep the last good file beside the new one, so a bad edit can be undone.
                try (FileInputStream in = new FileInputStream(f); FileOutputStream out = new FileOutputStream(bak)) {
                    byte[] b = new byte[8192];
                    for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
                }
            }
            if (!tmp.renameTo(f)) {
                // renameTo does not replace on every filesystem: remove first, then rename.
                if (!f.delete() || !tmp.renameTo(f)) throw new IOException("rename failed");
            }
            return true;
        } catch (IOException | RuntimeException e) {
            DiagLog.w("touch", "could not save touch.cfg: " + e.getMessage());
            tmp.delete();
            return false;
        }
    }

    /** Restores the previous good configuration, keeping the current one as the new backup. */
    boolean restoreBackup(Context c) {
        File bak = backupFile(c);
        if (!bak.isFile()) return false;
        try (FileInputStream in = new FileInputStream(bak)) {
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            byte[] b = new byte[4096];
            for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n);
            String text = o.toString("UTF-8");
            v.clear();
            for (String line : text.split("\n")) {
                String s = line.trim();
                int eq = s.indexOf('=');
                if (eq > 0) v.put(s.substring(0, eq), s.substring(eq + 1));
            }
            problem = null;
            return save(c);
        } catch (IOException | RuntimeException e) {
            DiagLog.w("touch", "could not restore the backup: " + e.getMessage());
            return false;
        }
    }

    String snapshot() {
        StringBuilder s = new StringBuilder();
        for (Map.Entry<String, String> e : v.entrySet()) s.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        return s.toString();
    }

    void restoreSnapshot(String text) {
        v.clear();
        for (String line : text.split("\n")) {
            int eq = line.indexOf('=');
            if (eq > 0) v.put(line.substring(0, eq), line.substring(eq + 1));
        }
    }

    /* ---- typed access ---------------------------------------------------------------- */

    float f(String key, float def) {
        String s = v.get(key);
        if (s == null) return def;
        try { return Float.parseFloat(s); } catch (NumberFormatException e) { return def; }
    }

    int i(String key, int def) {
        String s = v.get(key);
        if (s == null) return def;
        try { return Integer.parseInt(s.trim()); } catch (NumberFormatException e) { return def; }
    }

    String s(String key, String def) {
        String s = v.get(key);
        return s != null ? s : def;
    }

    void set(String key, String value) { v.put(key, value); }
    void setF(String key, float value) { v.put(key, String.format(java.util.Locale.US, "%.4f", value)); }
    void setI(String key, int value) { v.put(key, Integer.toString(value)); }

    void setCtl(String layout, String ctl, float x, float y, float size) {
        setF("l." + layout + "." + ctl + ".x", x);
        setF("l." + layout + "." + ctl + ".y", y);
        setF("l." + layout + "." + ctl + ".size", size);
    }

    String layout() { return s("layout", "standard"); }
    String profile() { return s("profile", "default"); }
    boolean locked() { return i("lock", 1) != 0; }
    boolean swipeEnabled() { return i("swipe.enabled", 1) != 0; }

    /* ---- per-control access for the selected layout ----------------------------------- */

    String key(String ctl, String prop) { return "l." + layout() + "." + ctl + "." + prop; }
    float ctlX(String ctl) { return f(key(ctl, "x"), -1f); }
    float ctlY(String ctl) { return f(key(ctl, "y"), -1f); }
    float ctlSize(String ctl) { return f(key(ctl, "size"), 1f); }
    float ctlOpacity(String ctl) { return f(key(ctl, "opacity"), 1f); }
    boolean ctlVisible(String ctl) { return i(key(ctl, "visible"), 1) != 0; }
    boolean ctlRemoved(String ctl) { return i(key(ctl, "removed"), 0) != 0; }

    /** Editing a preset layout makes it a Custom one, so a named preset never changes silently. */
    void detachFromPreset() {
        String l = layout();
        if ("standard".equals(l) || "compact".equals(l) || "swipe".equals(l)) {
            // Copy the preset's values into Custom 1 (or 2 when 1 is in use) and switch to it.
            String target = "custom1";
            for (String k : CTL_IDS) {
                for (String p : new String[] { "x", "y", "size", "opacity", "visible", "removed" })
                    v.put("l." + target + "." + k + "." + p, s("l." + l + "." + k + "." + p, "1"));
            }
            set("layout", target);
            DiagLog.i("touch", "layout preset " + l + " copied to " + target + " for editing");
        }
    }

    void resetLayout() {
        TouchConfig d = defaults();
        String l = layout();
        for (String k : CTL_IDS)
            for (String p : new String[] { "x", "y", "size", "opacity", "visible", "removed" })
                v.put("l." + l + "." + k + "." + p, d.s("l." + l + "." + k + "." + p, "1"));
        if ("standard".equals(l) || "compact".equals(l) || "swipe".equals(l)) {
            // Put the preset's own geometry back too (defaults() only carries the overrides).
            TouchConfig preset = defaults();
            for (String k : CTL_IDS)
                for (String p : new String[] { "x", "y", "size", "opacity", "visible", "removed" })
                    v.put("l." + l + "." + k + "." + p, preset.s("l." + l + "." + k + "." + p, "1"));
        }
    }

    void resetProfile() {
        TouchConfig d = defaults();
        String p = profile();
        for (String prop : new String[] { "deadzone", "sensitivity", "range", "progressive", "floating",
                "smooth", "invertx", "inverty", "sensx", "sensy" })
            v.put("p." + p + "." + prop, d.s("p." + p + "." + prop, "0"));
    }
}
