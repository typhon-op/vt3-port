package net.vt3port.game;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

/**
 * Serves the generated report to Android's share sheet. A report written by the app cannot be
 * shared as a {@code file://} URI (the platform refuses it), so the cache copy is exposed through
 * this provider instead. It can only ever read a plain file inside the app's reports directory.
 */
public final class DiagFileProvider extends ContentProvider {
    public static final String AUTHORITY = "net.vt3port.game.diagfiles";

    private static File reportsDir(Context c) {
        File dir = new File(c.getCacheDir(), "reports");
        if (!dir.isDirectory()) dir.mkdirs();
        return dir;
    }

    /** The shareable URI for a report already written by {@link #write}. */
    public static Uri uriFor(Context c, String name) {
        return new Uri.Builder().scheme("content").authority(AUTHORITY).appendPath(name).build();
    }

    /** Writes the report into the shareable cache directory (replacing an older copy). */
    public static File write(Context c, String name, String text) throws IOException {
        File dir = reportsDir(c);
        File f = new File(dir, name);
        try (java.io.Writer w = new java.io.OutputStreamWriter(
                new java.io.FileOutputStream(f), java.nio.charset.StandardCharsets.UTF_8)) {
            w.write(text);
        }
        return f;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    private File resolve(Uri uri) {
        String name = uri.getLastPathSegment();
        if (name == null || name.contains("/") || name.equals(".") || name.equals("..")) return null;
        File dir = reportsDir(getContext());
        File f = new File(dir, name);
        try {
            if (!f.getCanonicalPath().startsWith(dir.getCanonicalPath() + File.separator)) return null;
        } catch (IOException e) {
            return null;
        }
        return f;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,
                        String sortOrder) {
        File f = resolve(uri);
        if (f == null || !f.isFile()) return null;
        String[] cols = projection != null ? projection
                : new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE };
        MatrixCursor cursor = new MatrixCursor(cols, 1);
        Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++) {
            if (OpenableColumns.SIZE.equals(cols[i])) row[i] = f.length();
            else row[i] = f.getName();
        }
        cursor.addRow(row);
        return cursor;
    }

    @Override
    public String getType(Uri uri) {
        return "text/plain";
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        File f = resolve(uri);
        if (f == null || !f.isFile()) throw new FileNotFoundException(String.valueOf(uri));
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("read-only");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read-only");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read-only");
    }
}
