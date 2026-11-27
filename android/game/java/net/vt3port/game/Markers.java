package net.vt3port.game;

import android.content.Context;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * The small KEY=VALUE files in the app's data root that the Java screens and the native runtime
 * share. The runtime reads them in android_entry.c (apply_settings) and the renderer polls the
 * thermal and anisotropy ones once a second, so a setting changed here reaches the game without a
 * second settings system being invented.
 *
 * One file per key, the value as plain text: readable with a file manager, and impossible to leave
 * half-parsed when a write is interrupted.
 */
final class Markers {
    private Markers() {}

    static File root(Context c) {
        File f = c.getExternalFilesDir(null);
        return f != null ? f : c.getFilesDir();
    }

    static String read(File root, String name, String def) {
        File f = new File(root, name);
        if (!f.isFile()) return def;
        try (InputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            byte[] b = new byte[256];
            for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n);
            String s = o.toString("UTF-8").trim();
            return s.isEmpty() ? def : s;
        } catch (IOException | RuntimeException e) {
            return def;
        }
    }

    static Integer readInt(File root, String name, Integer def) {
        String s = read(root, name, null);
        if (s == null) return def;
        try {
            return Integer.parseInt(s.trim());
        } catch (NumberFormatException e) {
            return def;
        }
    }

    static int intOr(File root, String name, int def) {
        Integer v = readInt(root, name, null);
        return v != null ? v : def;
    }

    static boolean flag(File root, String name, boolean def) {
        String s = read(root, name, null);
        if (s == null) return def;
        return "1".equals(s) || "true".equalsIgnoreCase(s);
    }

    /** Write a marker, or delete it when value is null (which means "follow the preset/device"). */
    static void write(File root, String name, String value) {
        File f = new File(root, name);
        try {
            if (value == null) {
                f.delete();
                return;
            }
            try (FileOutputStream out = new FileOutputStream(f)) {
                out.write(value.getBytes("UTF-8"));
                out.getFD().sync();
            }
        } catch (IOException e) {
            DiagLog.w("settings", "could not write " + name + ": " + e.getMessage());
        }
    }

    static void writeInt(File root, String name, int value) {
        write(root, name, Integer.toString(value));
    }

    static void writeFlag(File root, String name, boolean value) {
        write(root, name, value ? "1" : "0");
    }
}
