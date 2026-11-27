package net.vt3port.game;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.Path;

/**
 * Copying, measuring and removing a real folder tree: the whole non-Android half of the installer's
 * folder route, with byte progress and cancellation, so it runs in the host test as well.
 *
 * A failed or cancelled copy removes the destination it was writing, never the source: the player's
 * own game folder is only ever read.
 */
final class TreeFiles {
    static final int MAX_DEPTH = 32;
    /* Counting stops here: a mis-picked "/" must not walk the whole device before the gate answers. */
    static final long MAX_SCAN_BYTES = 64L << 30;
    static final int CHUNK = 1 << 16;

    /** Called with the bytes copied so far; total is 0 when it is not known. */
    interface Progress {
        void at(long bytes, long total);
    }

    /** Polled during a copy and a SAF walk; true aborts the install. */
    interface Cancel {
        boolean cancelled();
    }

    private TreeFiles() {}

    /**
     * Copies src into dst (creating it). On any failure -- a full disk, a revoked grant, the player
     * cancelling -- whatever was copied into dst is removed again and the exception is rethrown.
     */
    static long copy(File src, File dst, long total, Progress p, Cancel c) throws IOException {
        long[] bytes = { 0 };
        try {
            copyDir(src, dst, 0, total, bytes, p, c);
        } catch (IOException | RuntimeException e) {
            delete(dst);
            throw e;
        }
        return bytes[0];
    }

    private static void copyDir(File src, File dst, int depth, long total, long[] bytes,
                                Progress p, Cancel c) throws IOException {
        if (depth > MAX_DEPTH) throw new IOException("That folder is nested too deeply.");
        if (c != null && c.cancelled()) throw new IOException("Install cancelled.");
        File[] all = src.listFiles();
        if (all == null) throw new IOException("Cannot read " + src.getName());
        if (!dst.isDirectory() && !dst.mkdirs() && !dst.isDirectory())
            throw new IOException("Cannot create " + dst);
        for (File f : all) {
            if (c != null && c.cancelled()) throw new IOException("Install cancelled.");
            File out = new File(dst, f.getName());
            if (isLink(f)) continue;              /* a link is not game data, and may point anywhere */
            if (f.isDirectory()) {
                copyDir(f, out, depth + 1, total, bytes, p, c);
            } else if (f.isFile()) {
                try (InputStream in = new FileInputStream(f);
                     OutputStream o = new FileOutputStream(out)) {
                    byte[] b = new byte[CHUNK];
                    long last = bytes[0];
                    for (int n; (n = in.read(b)) > 0; ) {
                        o.write(b, 0, n);
                        bytes[0] += n;
                        if (p != null && bytes[0] - last >= (8 << 20)) {
                            last = bytes[0];
                            p.at(bytes[0], total);
                        }
                    }
                    o.flush();
                }
                if (p != null) p.at(bytes[0], total);
            }
            /* Anything else (a fifo, a socket, a device node) is not game data: skip it. */
        }
    }

    /** Bytes under dir, capped; 0 when it cannot be read. Never follows a symlink. */
    static long bytes(File dir) {
        long[] sum = { 0 };
        walk(dir, sum, 0);
        return sum[0];
    }

    private static void walk(File dir, long[] sum, int depth) {
        if (dir == null || depth > MAX_DEPTH || sum[0] >= MAX_SCAN_BYTES) return;
        File[] all = dir.listFiles();
        if (all == null) return;
        for (File f : all) {
            if (sum[0] >= MAX_SCAN_BYTES) return;
            if (isLink(f)) continue;
            if (f.isDirectory()) {
                walk(f, sum, depth + 1);
            } else if (f.isFile()) {
                sum[0] += f.length();
            }
        }
    }

    /** Removes a tree. A symlink is removed, never followed. True when nothing is left. */
    static boolean delete(File f) {
        if (f == null || !f.exists()) return true;
        if (isLink(f)) return f.delete();
        if (f.isDirectory()) {
            File[] all = f.listFiles();
            if (all != null) for (File c : all) delete(c);
        }
        return f.delete();
    }

    /**
     * Whether this folder can be written by the app, tested by writing: Android's
     * {@code canWrite()} answers for the mount and not for the directory, and a read-only SAF grant
     * produces a folder that reads perfectly and cannot hold vt3.ini or a save.
     */
    static boolean writable(File dir) {
        if (dir == null || !dir.isDirectory()) return false;
        File probe = new File(dir, ".vt3_write_test");
        try {
            if (probe.exists()) probe.delete();
            try (FileOutputStream o = new FileOutputStream(probe)) {
                o.write(1);
            }
            return probe.delete();
        } catch (IOException | SecurityException e) {
            probe.delete();
            return false;
        }
    }

    private static boolean isLink(File f) {
        try {
            Path p = f.toPath();
            return Files.isSymbolicLink(p);
        } catch (Throwable e) {
            return false;
        }
    }
}
