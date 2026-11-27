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
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Storage access for the installer: the All Files Access gate, the Storage Access Framework
 * fallback, and the folder copy that puts game data where the native runtime can read it.
 *
 * The runtime opens plain POSIX paths, so a folder is read in place only when it has a real path the
 * app may also write (see {@link InstallPlan}); a tree that only exists behind a content:// URI is
 * copied into real storage first. The two routes agree on what a valid pick is because both hand the
 * folder to {@link GameFolder}.
 */
final class StorageAccess {
    /* Only used to decide what the screen offers, never to assume a grant. */
    static final String PREFS = "vt3_storage";
    static final String ASKED = "asked_all_files";
    static final String SOURCE = "source_path";
    static final String DEST = "dest_path";

    /* A hard ceiling on a picked tree, so a mis-tap on "/" cannot walk the whole device. */
    static final int MAX_ENTRIES = 200000;

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

    /** The data root last chosen from a real path, remembered across launches. */
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
     * A grant kept for the next launch, when the provider allows it. Not every provider does, so a
     * failure is logged and the install goes ahead on the grant the picker already gave this
     * activity; the caller checks {@link #listable(Context, Uri)} rather than assuming.
     */
    static void remember(Context c, Uri tree, Intent grant) {
        int flags = grant == null ? 0 : grant.getFlags()
                & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        int take = flags & Intent.FLAG_GRANT_READ_URI_PERMISSION;
        if (take == 0) {
            DiagLog.i("storage", "picker returned no read grant for " + tree);
            return;
        }
        try {
            c.getContentResolver().takePersistableUriPermission(tree, take);
        } catch (SecurityException e) {
            /* Not persistable on this provider: the grant still lasts for this activity. */
            DiagLog.i("storage", "this provider does not keep the grant for " + tree
                    + ": using the picker's access for this install");
        }
    }

    static void forget(Context c, Uri tree) {
        if (tree == null) return;
        try {
            c.getContentResolver().releasePersistableUriPermission(tree,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Throwable ignored) {
            /* nothing persisted: nothing to release */
        }
    }

    /** A directory the process can actually open and list right now. */
    static boolean readable(File dir) {
        return GameFolder.readable(dir);
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

    /* ---- SAF trees: a real path when there is one, a listing otherwise ---- */

    /**
     * The real POSIX path behind a picked tree, when the provider is Android's own external storage
     * provider. This is what makes a picked folder usable in place: the native runtime opens paths,
     * not content:// URIs. Returns null for any other provider, or when the path is not readable
     * (which is what happens without All Files Access, since scoped storage hides the folder).
     */
    static File treePath(Uri tree) {
        if (tree == null) return null;
        String authority = tree.getAuthority();
        if (authority == null || !authority.equals("com.android.externalstorage.documents")) return null;
        String id;
        try {
            id = DocumentsContract.getTreeDocumentId(tree);
        } catch (IllegalArgumentException e) {
            return null;
        }
        if (id == null) return null;
        int colon = id.indexOf(':');
        String volume = colon < 0 ? id : id.substring(0, colon);
        String rel = colon < 0 ? "" : id.substring(colon + 1);
        File base;
        if (volume.equalsIgnoreCase("primary")) {
            base = Environment.getExternalStorageDirectory();
        } else if (volume.equalsIgnoreCase("home")) {
            base = new File(Environment.getExternalStorageDirectory(), Environment.DIRECTORY_DOCUMENTS);
        } else {
            base = new File("/storage/" + volume);
        }
        File dir = rel.isEmpty() ? base : new File(base, rel);
        if (!readable(dir)) {
            DiagLog.i("storage", "tree " + tree + " maps to " + dir + ", which this app cannot read");
            return null;
        }
        return dir;
    }

    /** The picker's view of a tree, so the same folder rule runs on a content:// tree. */
    static final class SafListing implements GameFolder.Listing {
        private final ContentResolver cr;
        private final Uri tree;
        private final Map<String, List<Entry>> cache = new HashMap<>();

        static final class Entry {
            final String name, id;
            final boolean dir;

            Entry(String name, String id, boolean dir) {
                this.name = name;
                this.id = id;
                this.dir = dir;
            }
        }

        SafListing(Context c, Uri tree) {
            this.cr = c.getContentResolver();
            this.tree = tree;
        }

        private List<Entry> entries(String path) {
            if (cache.containsKey(path)) return cache.get(path);
            List<Entry> out = null;
            String parent = path == null || path.isEmpty() ? rootId() : idOf(path);
            if (parent != null) {
                Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent);
                String[] cols = { DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                        DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                        DocumentsContract.Document.COLUMN_MIME_TYPE };
                out = new ArrayList<>();
                try (Cursor cur = cr.query(children, cols, null, null, null)) {
                    if (cur != null) while (cur.moveToNext()) {
                        String id = cur.getString(0), name = cur.getString(1), mime = cur.getString(2);
                        if (name == null || id == null || name.contains("/") || name.equals(".")
                                || name.equals("..")) continue;
                        out.add(new Entry(name, id,
                                DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)));
                    }
                } catch (SecurityException | IllegalArgumentException e) {
                    out = null;   /* revoked or gone: the caller reports it as unreadable */
                }
            }
            cache.put(path, out);
            return out;
        }

        private String rootId() {
            try {
                return DocumentsContract.getTreeDocumentId(tree);
            } catch (IllegalArgumentException e) {
                return null;
            }
        }

        /** The document id of one child by name, ignoring case; null when it is not there. */
        String idOf(String path) {
            int slash = path.indexOf('/');
            if (slash < 0) return childId("", path);
            String parent = childId("", path.substring(0, slash));
            return parent == null ? null : childId(path.substring(0, slash), path.substring(slash + 1));
        }

        String childId(String path, String name) {
            List<Entry> list = entries(path);
            if (list == null) return null;
            for (Entry e : list) if (e.name.equalsIgnoreCase(name)) return e.id;
            return null;
        }

        @Override
        public String[] names(String path) {
            List<Entry> list = entries(path);
            if (list == null) return null;
            String[] out = new String[list.size()];
            for (int i = 0; i < list.size(); i++) out[i] = list.get(i).name;
            return out;
        }

        @Override
        public boolean isDir(String path, String name) {
            List<Entry> list = entries(path);
            if (list == null) return false;
            for (Entry e : list) if (e.name.equalsIgnoreCase(name)) return e.dir;
            return false;
        }
    }

    /* ---- SAF tree copy: the no-path route, into real storage ---- */

    /**
     * Copies a picked tree into dst, or only the one child named by {@code child} (the folder that
     * holds Resource). Throws with a message fit for the setup screen on a revoked grant, a
     * disappeared folder, a full disk, a cancellation or a tree that is implausibly large.
     */
    static long copyTree(Context c, Uri tree, String child, File dst,
                         TreeFiles.Progress p, TreeFiles.Cancel cancel) throws IOException {
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
        String startId = rootId;
        if (child != null && !child.isEmpty()) {
            startId = new SafListing(c, tree).childId("", child);
            if (startId == null) throw new IOException("That folder changed. Pick it again.");
        }
        if (!dst.isDirectory() && !dst.mkdirs() && !dst.isDirectory())
            throw new IOException("Cannot create " + dst);
        long[] state = new long[] { 0, 0 };
        copyChildren(cr, tree, startId, dst, p, state, cancel);
        if (state[0] == 0) throw new IOException("That folder is empty.");
        return state[1];
    }

    private static void copyChildren(ContentResolver cr, Uri tree, String parentId, File dst,
                                     TreeFiles.Progress p, long[] state, TreeFiles.Cancel cancel)
            throws IOException {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] cols = { DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE };
        try (Cursor cur = cr.query(children, cols, null, null, null)) {
            if (cur == null) throw new IOException("That folder stopped responding. Try again.");
            while (cur.moveToNext()) {
                if (cancel != null && cancel.cancelled()) throw new IOException("Install cancelled.");
                if (++state[0] > MAX_ENTRIES) throw new IOException("That folder has too many files.");
                String id = cur.getString(0), name = cur.getString(1), mime = cur.getString(2);
                if (name == null || id == null || name.contains("/") || name.equals(".")
                        || name.equals("..")) continue;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    File sub = new File(dst, name);
                    if (!sub.isDirectory() && !sub.mkdirs() && !sub.isDirectory())
                        throw new IOException("Cannot create " + sub);
                    copyChildren(cr, tree, id, sub, p, state, cancel);
                } else {
                    File out = new File(dst, name);
                    try (InputStream in = cr.openInputStream(
                            DocumentsContract.buildDocumentUriUsingTree(tree, id))) {
                        if (in == null) throw new IOException("Cannot read " + name);
                        try (OutputStream o = new FileOutputStream(out)) {
                            byte[] b = new byte[TreeFiles.CHUNK];
                            long last = 0;
                            for (int n; (n = in.read(b)) > 0; ) {
                                if (cancel != null && cancel.cancelled())
                                    throw new IOException("Install cancelled.");
                                o.write(b, 0, n);
                                state[1] += n;
                                if (p != null && state[1] - last > (8 << 20)) {
                                    last = state[1];
                                    p.at(state[1], 0);
                                }
                            }
                        }
                    }
                    if (p != null) p.at(state[1], 0);
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

}
