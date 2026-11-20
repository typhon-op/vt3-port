package net.vt3port.game;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Storage access for the installer: the All Files Access gate, the Storage Access Framework
 * fallback, and the folder copy that puts game data where the native runtime can read it.
 *
 * The runtime opens plain POSIX paths, so nothing it reads may stay behind a content:// URI. A
 * folder picked with the SAF picker is therefore always copied into real storage first; a folder
 * picked with All Files Access is a real path already and is used in place.
 */
final class StorageAccess {
    /* Only used to decide what the screen offers, never to assume a grant. */
    static final String PREFS = "vt3_storage";
    static final String ASKED = "asked_all_files";
    static final String SOURCE = "source_path";
    static final String DEST = "dest_path";

    /* A hard ceiling on a picked tree, so a mis-tap on "/" cannot walk the whole device. */
    static final int MAX_ENTRIES = 200000;

    interface Progress {
        void at(long bytes, String name);
    }

    private StorageAccess() {}

    static boolean allFilesSupported() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.R;
    }

    /**
     * The real answer to "may we read the whole shared volume", asked of the system every time.
     * Opening the settings screen is not evidence of anything, so this is never cached.
     */
    static boolean allFiles(Context c) {
        if (!allFilesSupported()) return false;
        try {
            return Environment.isExternalStorageManager();
        } catch (Throwable e) { /* some OEM builds throw here instead of returning false */
            return false;
        }
    }

    /** The per-app All Files Access page, with the whole-list page as a fallback. */
    static Intent settings(Context c) {
        Intent app = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:" + c.getPackageName()));
        if (app.resolveActivity(c.getPackageManager()) != null) return app;
        return new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
    }

    static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static boolean asked(Context c) {
        return prefs(c).getBoolean(ASKED, false);
    }

    static void markAsked(Context c) {
        prefs(c).edit().putBoolean(ASKED, true).apply();
    }

    static File sourcePath(Context c) {
        return path(prefs(c).getString(SOURCE, null));
    }

    static File destPath(Context c) {
        return path(prefs(c).getString(DEST, null));
    }

    static void setSourcePath(Context c, File f) {
        prefs(c).edit().putString(SOURCE, f == null ? null : f.getAbsolutePath()).apply();
    }

    static void setDestPath(Context c, File f) {
        prefs(c).edit().putString(DEST, f == null ? null : f.getAbsolutePath()).apply();
    }

    private static File path(String s) {
        return s == null || s.isEmpty() ? null : new File(s);
    }

    /**
     * A grant kept for the next launch. The system can drop it at any time (a clear-data, a
     * revoke), so callers must still check {@link #readable(File)} or query the tree afterwards.
     */
    static void remember(Context c, Uri tree, Intent grant) {
        int flags = grant == null ? 0 : grant.getFlags()
                & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        int take = flags & Intent.FLAG_GRANT_READ_URI_PERMISSION;
        if (take == 0) return; /* a read grant is the only thing a source folder needs */
        try {
            c.getContentResolver().takePersistableUriPermission(tree, take);
        } catch (SecurityException ignored) {
            /* not persistable on this provider: the grant still lasts for this activity */
        }
    }

    static boolean granted(Context c, Uri tree) {
        if (tree == null) return false;
        for (android.content.UriPermission p : c.getContentResolver().getPersistedUriPermissions())
            if (p.getUri().equals(tree) && p.isReadPermission()) return true;
        return false;
    }

    static void forget(Context c, Uri tree) {
        if (tree == null) return;
        try {
            c.getContentResolver().releasePersistableUriPermission(tree,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Throwable ignored) {
        }
    }

    /** A directory the process can actually open and list right now. */
    static boolean readable(File dir) {
        if (dir == null || !dir.isDirectory() || !dir.canRead()) return false;
        String[] list = dir.list();
        return list != null;
    }

    /** True when this directory is inside the app's own external files dir (no permission needed). */
    static boolean inAppStorage(Context c, File dir) {
        if (dir == null) return false;
        try {
            File base = c.getExternalFilesDir(null);
            if (base == null) return false;
            String b = base.getCanonicalPath();
            String d = dir.getCanonicalPath();
            return d.equals(b) || d.startsWith(b + File.separator);
        } catch (IOException e) {
            return false;
        }
    }

    static long freeBytes(File dir) {
        File probe = dir;
        while (probe != null && !probe.exists()) probe = probe.getParentFile();
        return probe == null ? 0 : probe.getUsableSpace();
    }

    /** Subdirectories for the in-app browser, alphabetically; never throws. */
    static List<File> dirs(File dir) {
        List<File> out = new ArrayList<>();
        File[] all = readable(dir) ? dir.listFiles() : null;
        if (all == null) return out;
        for (File f : all)
            if (f.isDirectory() && !f.getName().startsWith(".")) out.add(f);
        out.sort((a, b) -> a.getName().compareToIgnoreCase(b.getName()));
        return out;
    }

    /** The shared volume root, for the browser. */
    static File sharedRoot() {
        return Environment.getExternalStorageDirectory();
    }

    /* ---- SAF tree copy: the no-permission path, into real storage ---- */

    /**
     * Copies a picked tree into dst. Throws with a message fit for the setup screen on a revoked
     * grant, a disappeared folder, a full disk, or a tree that is implausibly large.
     */
    static long copyTree(Context c, Uri tree, File dst, Progress p) throws IOException {
        ContentResolver cr = c.getContentResolver();
        String rootId;
        try {
            rootId = DocumentsContract.getTreeDocumentId(tree);
        } catch (IllegalArgumentException e) {
            throw new IOException("That folder cannot be read. Pick it again.");
        }
        /* A query is the only honest test of the grant: it fails if it was revoked. */
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, rootId);
        Cursor probe = null;
        try {
            probe = cr.query(children, new String[] { DocumentsContract.Document.COLUMN_DOCUMENT_ID },
                    null, null, null);
            if (probe == null) throw new IOException("That folder cannot be read. Grant access again.");
        } catch (SecurityException e) {
            throw new IOException("Access to that folder was revoked. Pick it again.");
        } finally {
            if (probe != null) probe.close();
        }
        if (!dst.isDirectory() && !dst.mkdirs()) throw new IOException("Cannot create " + dst);
        long[] state = new long[] { 0, 0 };
        copyChildren(cr, tree, rootId, dst, p, state);
        if (state[0] == 0) throw new IOException("That folder is empty.");
        return state[1];
    }

    private static void copyChildren(ContentResolver cr, Uri tree, String parentId, File dst,
                                     Progress p, long[] state) throws IOException {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] cols = { DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE };
        try (Cursor cur = cr.query(children, cols, null, null, null)) {
            if (cur == null) throw new IOException("That folder stopped responding. Try again.");
            while (cur.moveToNext()) {
                if (++state[0] > MAX_ENTRIES) throw new IOException("That folder has too many files.");
                String id = cur.getString(0), name = cur.getString(1), mime = cur.getString(2);
                if (name == null || id == null || name.contains("/") || name.equals(".")
                        || name.equals("..")) continue;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    File sub = new File(dst, name);
                    if (!sub.isDirectory() && !sub.mkdirs()) throw new IOException("Cannot create " + sub);
                    copyChildren(cr, tree, id, sub, p, state);
                } else {
                    File out = new File(dst, name);
                    try (InputStream in = cr.openInputStream(
                            DocumentsContract.buildDocumentUriUsingTree(tree, id))) {
                        if (in == null) throw new IOException("Cannot read " + name);
                        try (OutputStream o = new FileOutputStream(out)) {
                            byte[] b = new byte[1 << 16];
                            long last = 0;
                            for (int n; (n = in.read(b)) > 0; ) {
                                o.write(b, 0, n);
                                state[1] += n;
                                if (p != null && state[1] - last > (8 << 20)) {
                                    last = state[1];
                                    p.at(state[1], name);
                                }
                            }
                        }
                    }
                    if (p != null) p.at(state[1], name);
                }
            }
        } catch (SecurityException e) {
            throw new IOException("Access to that folder was revoked. Pick it again.");
        }
    }

    /**
     * A live query against a picked tree. This is the only test that the grant still works: a grant
     * kept in the preferences can be revoked or its folder deleted between launches.
     */
    static boolean listable(Context c, Uri tree) {
        if (tree == null) return false;
        try {
            String id = DocumentsContract.getTreeDocumentId(tree);
            Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
            try (Cursor cur = c.getContentResolver().query(uri,
                    new String[] { DocumentsContract.Document.COLUMN_DOCUMENT_ID }, null, null, null)) {
                return cur != null;
            }
        } catch (Throwable e) {
            return false;
        }
    }

    /** A folder that looks like an unpacked PC install: the Resource tree the game reads. */
    static boolean looksInstalled(File dir) {
        return readable(dir) && new File(dir, "Resource").isDirectory();
    }
}
