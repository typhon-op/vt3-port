package net.vt3port.game;

import android.content.Context;
import android.net.Uri;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Adreno Tools: importing a custom Vulkan driver package and checking whether it could run here.
 *
 * What is real in this class: the import, the validation (a zip or a bare shared object, an
 * AArch64 ELF, the ABI and the file's SHA-256), the stored copy inside the app's own storage, the
 * metadata, and the backup/rollback of the previous import. None of it touches a system partition
 * and none of it needs root.
 *
 * What is NOT real, and is reported as such rather than faked: applying the driver. The native
 * runtime links Android's system Vulkan loader (libvulkan.so) at link time; it has no hook to
 * dlopen a driver from app storage and no Vulkan dispatch table of its own. Android also does not
 * let an app replace the system graphics driver. So an imported package is validated, kept and
 * offered back to the player, but {@link #applySupported()} stays false and the screen says why.
 * A build that adds per-app driver loading (an Adreno Tools style loader namespace) is the only
 * thing that would turn this into an applied driver.
 */
final class Adreno {
    static final String CURRENT = "driver_current";
    static final String BACKUP = "driver_backup";
    static final long MAX_BYTES = 256L << 20;

    private Adreno() {}

    static File dir(Context c) {
        return new File(Game.root(c), "drivers");
    }

    static File store(Context c, String name) {
        return new File(dir(c), name);
    }

    /** True when this build can actually load an imported driver. It cannot; see the class note. */
    static boolean applySupported() {
        return false;
    }

    static String limitation() {
        return "This build cannot load a custom driver: the native runtime is linked against "
                + "Android's system Vulkan loader and has no per-app driver loader, and Android "
                + "does not allow an app to replace the system driver. The package is validated "
                + "and kept on this device, and the system driver stays in use.";
    }

    /** The imported package currently selected, or null when the system driver is in use. */
    static String current(Context c) {
        String s = Markers.read(Game.root(c), CURRENT, null);
        if (s == null) return null;
        return new File(dir(c), s).isDirectory() ? s : null;
    }

    static String backup(Context c) {
        String s = Markers.read(Game.root(c), BACKUP, null);
        if (s == null) return null;
        return new File(dir(c), s).isDirectory() ? s : null;
    }

    /** The metadata line the screen shows: name, ABI, size and the digest recorded at import. */
    static String describe(Context c, String name) {
        if (name == null) return null;
        File d = store(c, name);
        return Markers.read(d, "info.txt", name);
    }

    /** The outcome of looking at a picked file. */
    static final class Result {
        String name;          /* the folder name it will be stored under */
        String file;          /* the shared object inside the package */
        String abi;           /* "arm64-v8a" when the ELF says AArch64 */
        String sha256;
        long bytes;
        String version = "not stated by the package";
        String problem;       /* null when the package is acceptable */
        boolean bareSo;       /* picked as a .so rather than a zip */

        boolean ok() {
            return problem == null;
        }
    }

    /**
     * Validates a picked package without writing anything: zip structure, a Vulkan shared object
     * inside it, and that object's ELF header. A package that fails says exactly what failed.
     */
    static Result inspect(Context c, Uri uri) {
        Result r = new Result();
        String display = displayName(c, uri);
        r.name = safeName(display);
        try (InputStream raw = c.getContentResolver().openInputStream(uri)) {
            if (raw == null) {
                r.problem = "That file cannot be opened.";
                return r;
            }
            byte[] head = readAtMost(raw, 4);
            if (head.length < 4) {
                r.problem = "That file is empty.";
                return r;
            }
            boolean zip = head[0] == 'P' && head[1] == 'K';
            if (zip) {
                inspectZip(c, uri, r);
            } else {
                r.bareSo = true;
                inspectSo(c, uri, r, display);
            }
            if (r.problem == null && !"arm64-v8a".equals(r.abi))
                r.problem = "That driver is not an AArch64 (arm64-v8a) library, so this app cannot "
                        + "use it. This port is built for arm64-v8a only.";
        } catch (SecurityException e) {
            r.problem = "That file cannot be read: " + e.getMessage();
        } catch (IOException e) {
            r.problem = "That file cannot be read: " + e.getMessage();
        }
        return r;
    }

    private static void inspectZip(Context c, Uri uri, Result r) throws IOException {
        String soName = null;
        long soBytes = 0;
        byte[] elf = null;
        byte[] meta = null;
        try (ZipInputStream z = new ZipInputStream(c.getContentResolver().openInputStream(uri))) {
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                String n = e.getName();
                String lower = n.toLowerCase(Locale.US);
                if (lower.endsWith("/libvulkan.so") || lower.equals("libvulkan.so")
                        || lower.endsWith("/vulkan.so") || lower.equals("vulkan.so")) {
                    byte[] b = readAtMost(z, 64);
                    if (elf == null) {
                        elf = b;
                        soName = n;
                    }
                } else if (lower.endsWith("meta.json") || lower.endsWith("driver.json")
                        || lower.endsWith("manifest.json")) {
                    meta = readAtMost(z, 4096);
                } else if (lower.endsWith(".so") && soName == null) {
                    /* Some packages name the library after the driver build, not libvulkan.so. */
                    byte[] b = readAtMost(z, 64);
                    if (isElf(b)) {
                        elf = b;
                        soName = n;
                    }
                }
            }
        }
        if (soName == null) {
            r.problem = "That zip holds no Vulkan driver library (libvulkan.so).";
            return;
        }
        r.file = soName;
        r.abi = elfAbi(elf);
        r.version = versionFrom(meta);
        r.sha256 = sha256(c, uri);
        r.bytes = sizeOf(c, uri);
    }

    private static void inspectSo(Context c, Uri uri, Result r, String display) throws IOException {
        byte[] elf;
        try (InputStream in = c.getContentResolver().openInputStream(uri)) {
            elf = readAtMost(in, 64);
        }
        if (!isElf(elf)) {
            r.problem = "That file is not a shared library: a driver package is a .zip (or the "
                    + "libvulkan.so inside one).";
            return;
        }
        r.file = display == null ? "libvulkan.so" : display;
        r.abi = elfAbi(elf);
        r.sha256 = sha256(c, uri);
        r.bytes = sizeOf(c, uri);
    }

    private static boolean isElf(byte[] b) {
        return b != null && b.length >= 20 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F';
    }

    /** "arm64-v8a" for a 64-bit little-endian AArch64 ELF, else a description of what it is. */
    private static String elfAbi(byte[] b) {
        if (!isElf(b)) return "not an ELF library";
        boolean is64 = b[4] == 2;
        boolean le = b[5] == 1;
        int machine = (b[18] & 0xff) | ((b[19] & 0xff) << 8);
        if (is64 && le && machine == 0xB7) return "arm64-v8a";
        if (machine == 0x28) return "armeabi-v7a";
        if (machine == 0x3E) return "x86_64";
        return String.format(Locale.US, "unsupported (ELF machine 0x%x)", machine);
    }

    /** A short version string from a package's own metadata, when it carries one. */
    private static String versionFrom(byte[] meta) {
        if (meta == null) return "not stated by the package";
        String s = new String(meta, java.nio.charset.StandardCharsets.UTF_8);
        for (String key : new String[] { "version", "driverVersion", "driver_version" }) {
            int i = s.indexOf('"' + key + '"');
            if (i < 0) continue;
            int colon = s.indexOf(':', i);
            if (colon < 0) continue;
            int q1 = s.indexOf('"', colon + 1);
            int q2 = q1 < 0 ? -1 : s.indexOf('"', q1 + 1);
            if (q1 > 0 && q2 > q1) return s.substring(q1 + 1, q2);
        }
        return "not stated by the package";
    }

    /**
     * Stores a validated package: the file, an info.txt with everything the screen shows, and the
     * markers. The previously selected package becomes the backup, so a rollback is one tap.
     */
    static String save(Context c, Uri uri, Result r) throws IOException {
        File root = dir(c);
        if (!root.isDirectory() && !root.mkdirs() && !root.isDirectory())
            throw new IOException("Cannot create " + root);
        File dest = new File(root, r.name);
        if (dest.exists() && !TreeFiles.delete(dest))
            throw new IOException("Cannot replace the earlier copy at " + dest);
        if (!dest.mkdirs() && !dest.isDirectory()) throw new IOException("Cannot create " + dest);
        File out = new File(dest, r.bareSo ? r.file : "driver.zip");
        try (InputStream in = c.getContentResolver().openInputStream(uri);
             FileOutputStream o = new FileOutputStream(out)) {
            if (in == null) throw new IOException("That file cannot be opened.");
            byte[] b = new byte[64 * 1024];
            for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n);
            o.getFD().sync();
        }
        String info = r.name + "\nABI: " + r.abi + "\nLibrary: " + r.file
                + "\nVersion: " + r.version + "\nSize: " + bytes(r.bytes)
                + "\nSHA-256: " + r.sha256
                + "\nStatus: imported and validated; not applied by this build";
        Markers.write(dest, "info.txt", info);
        Markers.write(dest, "source.txt", uri.toString());

        String previous = current(c);
        if (previous != null && !previous.equals(r.name))
            Markers.write(Game.root(c), BACKUP, previous);
        Markers.write(Game.root(c), CURRENT, r.name);
        DiagLog.i("driver", "driver package " + r.name + " imported: " + r.abi + ", " + r.file
                + ", " + bytes(r.bytes) + ", sha256 " + r.sha256);
        return r.name;
    }

    /** Back to the system driver: the imports stay on disk, only the selection is cleared. */
    static void useSystem(Context c) {
        String previous = current(c);
        Markers.write(Game.root(c), CURRENT, null);
        if (previous != null) Markers.write(Game.root(c), BACKUP, previous);
        DiagLog.i("driver", "system driver selected; imported packages kept");
    }

    /** Swap the current and previous imports, so the last one can be brought back. */
    static boolean rollback(Context c) {
        String cur = current(c), prev = backup(c);
        if (prev == null) return false;
        Markers.write(Game.root(c), CURRENT, prev);
        Markers.write(Game.root(c), BACKUP, cur);
        DiagLog.i("driver", "rolled back to " + prev + " (was " + cur + ")");
        return true;
    }

    /** Remove every imported package. The system driver is untouched. */
    static boolean clearAll(Context c) {
        boolean ok = TreeFiles.delete(dir(c));
        Markers.write(Game.root(c), CURRENT, null);
        Markers.write(Game.root(c), BACKUP, null);
        DiagLog.i("driver", "imported driver packages removed");
        return ok;
    }

    static List<String> list(Context c) {
        List<String> out = new ArrayList<>();
        File[] fs = dir(c).listFiles();
        if (fs == null) return out;
        for (File f : fs) if (f.isDirectory()) out.add(f.getName());
        java.util.Collections.sort(out);
        return out;
    }

    static String bytes(long n) {
        if (n >= 1 << 20) return String.format(Locale.US, "%.1f MB", n / 1048576.0);
        if (n >= 1 << 10) return String.format(Locale.US, "%.0f KB", n / 1024.0);
        return n + " B";
    }

    /* ---- small I/O helpers ---- */

    private static byte[] readAtMost(InputStream in, int max) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] b = new byte[256];
        int total = 0;
        for (int n; total < max && (n = in.read(b, 0, Math.min(b.length, max - total))) > 0; ) {
            o.write(b, 0, n);
            total += n;
        }
        return o.toByteArray();
    }

    private static String sha256(Context c, Uri uri) {
        try (InputStream in = c.getContentResolver().openInputStream(uri)) {
            if (in == null) return "unavailable";
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] b = new byte[64 * 1024];
            long total = 0;
            for (int n; (n = in.read(b)) > 0; ) {
                total += n;
                if (total > MAX_BYTES) return "too large";
                md.update(b, 0, n);
            }
            StringBuilder s = new StringBuilder();
            for (byte x : md.digest()) s.append(String.format("%02x", x));
            return s.toString();
        } catch (Exception e) {
            return "unavailable";
        }
    }

    private static long sizeOf(Context c, Uri uri) {
        try (InputStream in = c.getContentResolver().openInputStream(uri)) {
            if (in == null) return 0;
            byte[] b = new byte[64 * 1024];
            long total = 0;
            for (int n; (n = in.read(b)) > 0; ) {
                total += n;
                if (total > MAX_BYTES) return MAX_BYTES;
            }
            return total;
        } catch (Exception e) {
            return 0;
        }
    }

    private static String displayName(Context c, Uri uri) {
        try (android.database.Cursor cur = c.getContentResolver().query(uri,
                new String[] { android.provider.OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (cur != null && cur.moveToFirst() && !cur.isNull(0)) return cur.getString(0);
        } catch (RuntimeException ignored) {
        }
        String p = uri.getLastPathSegment();
        return p == null ? "driver" : p;
    }

    private static String safeName(String display) {
        String base = display == null ? "driver" : display;
        int dot = base.lastIndexOf('.');
        if (dot > 0) base = base.substring(0, dot);
        StringBuilder s = new StringBuilder();
        for (char ch : base.toCharArray())
            s.append(Character.isLetterOrDigit(ch) || ch == '-' || ch == '_' ? ch : '-');
        String out = s.toString();
        if (out.isEmpty()) out = "driver";
        return out.length() > 48 ? out.substring(0, 48) : out;
    }
}
