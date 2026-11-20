package net.vt3port.game;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Launcher. On first run the player picks their PC disc image (or an already-unpacked game folder)
 * and the v1.0.1 VT3.exe; this unpacks or copies the game data and builds image.bin in the app's
 * files dir (the VT3_ANDROID_ROOT the game reads), then writes install.ok. Every later launch goes
 * straight to the game.
 *
 * Storage: the picker alone is enough, because whatever it returns is copied into real storage the
 * runtime can open. All Files Access is optional and only adds two shortcuts — reading an unpacked
 * folder where it lies, and installing to a folder of the player's choosing. No code path here reads
 * a content:// URI as if it were a path, and no install writes install.ok without first proving the
 * data it wrote is complete and readable.
 *
 * The screen is res/layout/activity_installer.xml: a court banner with the VT3 wordmark, one card
 * per file the player supplies, a storage card, the install button, and a foot strip for progress.
 */
public class Installer extends Activity {
    static { System.loadLibrary("vt3install"); }

    static native int extractIso(int fd, String outDir);
    static native int progress(); /* permille of cabinet files done */
    static native byte[] flattenPe(byte[] exe);

    /* SHA-256 of the image built from the v1.0.1 VT3.exe the port is translated from. */
    static final String IMAGE_SHA256 = "52be3cda5edb9aa536394fed656e22270d688d74e86db8602fd39a7512fc80bf";
    static final int PICK_ISO = 1, PICK_ZIP = 2, PICK_TREE = 3;
    /* Room the unpacked game needs, with headroom for the image and the shader pack. */
    static final long NEED_BYTES = 5L << 30;
    static final int MAX_DEPTH = 32;

    File root;
    Uri iso, zip, sourceTree;
    File sourceDir, destDir;
    Button isoButton, zipButton, installButton, storageButton, sourceButton, destButton;
    TextView isoState, exeState, status, storageState, sourceState, destState;
    ProgressBar bar;
    static final Handler ui = new Handler(Looper.getMainLooper());
    /* The install thread outlives the activity (a recreate, or a relaunch from the icon), so it
     * reports to whichever instance is on screen, and a second install never runs over the first. */
    static volatile Installer live;
    static volatile boolean busy;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        root = getExternalFilesDir(null);
        if (new File(root, "install.ok").exists()) {
            launch();
            return;
        }
        setContentView(R.layout.activity_installer);
        isoButton = findViewById(R.id.iso_button);
        zipButton = findViewById(R.id.exe_button);
        installButton = findViewById(R.id.install_button);
        isoState = findViewById(R.id.iso_state);
        exeState = findViewById(R.id.exe_state);
        status = findViewById(R.id.status);
        bar = findViewById(R.id.progress);
        storageButton = findViewById(R.id.storage_button);
        sourceButton = findViewById(R.id.source_button);
        destButton = findViewById(R.id.dest_button);
        storageState = findViewById(R.id.storage_state);
        sourceState = findViewById(R.id.source_state);
        destState = findViewById(R.id.dest_state);
        isoButton.setOnClickListener(v -> pick(PICK_ISO));
        zipButton.setOnClickListener(v -> pick(PICK_ZIP));
        installButton.setOnClickListener(v -> install());
        storageButton.setOnClickListener(v -> askAllFiles());
        sourceButton.setOnClickListener(v -> pickSource());
        destButton.setOnClickListener(v -> pickDestination());
        /* A remembered folder can be gone: unmounted card, app data cleared, folder deleted. */
        sourceDir = StorageAccess.sourcePath(this);
        if (sourceDir != null && !StorageAccess.looksInstalled(sourceDir)) {
            StorageAccess.setSourcePath(this, null);
            sourceDir = null;
        }
        destDir = StorageAccess.destPath(this);
        live = this;
        if (busy) {
            isoButton.setEnabled(false);
            zipButton.setEnabled(false);
            status.setText(R.string.setup_running);
        }
        refreshStorage();
    }

    @Override
    protected void onResume() {
        super.onResume();
        /* Coming back from the system settings screen proves nothing by itself, so the grant is
         * asked of the system again here, every time. */
        refreshStorage();
    }

    @Override
    protected void onDestroy() {
        if (live == this) live = null;
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (!busy) super.onBackPressed(); /* leaving mid-install lets the system kill the unpacking */
    }

    void pick(int code) {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), code);
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        if (result != RESULT_OK || data == null) return;
        if (code == PICK_ISO) {
            iso = data.getData();
            isoButton.setText(R.string.step1_done);
            isoState.setText(R.string.step1_done);
            isoState.setTextColor(getColor(R.color.vt3_ball));
        }
        if (code == PICK_ZIP) {
            zip = data.getData();
            zipButton.setText(R.string.step2_done);
            exeState.setText(R.string.step2_done);
            exeState.setTextColor(getColor(R.color.vt3_ball));
        }
        if (code == PICK_TREE) {
            Uri tree = data.getData();
            if (tree == null) return;
            StorageAccess.remember(this, tree, data);
            /* Remembering is not the same as being allowed: ask the provider, now. */
            if (StorageAccess.granted(this, tree) && StorageAccess.listable(this, tree)) {
                sourceTree = tree;
                sourceDir = null;
                StorageAccess.setSourcePath(this, null);
                status.setText(R.string.source_tree_ready);
            } else {
                StorageAccess.forget(this, tree);
                sourceTree = null;
                status.setText(R.string.source_tree_failed);
            }
        }
        refreshStorage();
    }

    /* ---- storage UI ---- */

    void refreshStorage() {
        if (storageButton == null || busy) return;
        boolean all = StorageAccess.allFiles(this);
        storageState.setText(all ? R.string.storage_granted
                : StorageAccess.asked(this) ? R.string.storage_denied_asked : R.string.storage_denied);
        storageState.setTextColor(getColor(all ? R.color.vt3_ball : R.color.vt3_muted));
        storageButton.setText(all ? R.string.storage_check : R.string.storage_allow);
        if (sourceDir != null) {
            sourceState.setText(getString(R.string.source_using, sourceDir.getAbsolutePath()));
        } else if (sourceTree != null) {
            if (StorageAccess.granted(this, sourceTree) && StorageAccess.listable(this, sourceTree)) {
                sourceState.setText(R.string.source_tree);
            } else {
                /* Revoked or deleted since it was picked: say so instead of failing at the end. */
                StorageAccess.forget(this, sourceTree);
                sourceTree = null;
                sourceState.setText(R.string.source_tree_lost);
                status.setText(R.string.source_tree_lost);
            }
        } else {
            sourceState.setText(R.string.source_disc);
        }
        destState.setText(destDir == null ? getString(R.string.dest_default) : destDir.getAbsolutePath());
        installButton.setEnabled(canInstall());
    }

    boolean canInstall() {
        boolean haveGame = iso != null || sourceDir != null || sourceTree != null;
        return !busy && haveGame && zip != null;
    }

    /** The system settings screen, opened only because the player asked for it. */
    void askAllFiles() {
        if (busy) return;
        if (!StorageAccess.allFilesSupported()) {
            status.setText(R.string.storage_old);
            return;
        }
        if (StorageAccess.allFiles(this)) { /* already allowed: just re-read the state */
            refreshStorage();
            return;
        }
        StorageAccess.markAsked(this);
        try {
            startActivity(StorageAccess.settings(this));
            status.setText(R.string.storage_howto);
        } catch (Throwable e) {
            status.setText(R.string.storage_no_settings);
        }
    }

    void pickSource() {
        if (busy) return;
        if (StorageAccess.allFiles(this)) {
            browse(StorageAccess.sharedRoot(), dir -> {
                if (!StorageAccess.looksInstalled(dir)) {
                    status.setText(getString(R.string.source_not_install, dir.getAbsolutePath()));
                    return;
                }
                sourceDir = dir;      /* a real path: the runtime reads it in place, nothing copied */
                sourceTree = null;
                StorageAccess.setSourcePath(this, dir);
                status.setText(getString(R.string.source_using, dir.getAbsolutePath()));
                refreshStorage();
            });
            return;
        }
        /* No broad access: the picker's own grant is enough, and the copy lands in real storage. */
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_TREE);
        } catch (Throwable e) {
            status.setText(R.string.storage_no_picker);
        }
    }

    void pickDestination() {
        if (busy) return;
        if (!StorageAccess.allFiles(this)) {
            /* Said, not silently ignored; the settings button beside it is the way forward. */
            status.setText(R.string.dest_needs_access);
            return;
        }
        browse(StorageAccess.sharedRoot(), dir -> {
            if (StorageAccess.freeBytes(dir) < NEED_BYTES) {
                status.setText(R.string.dest_no_space);
                return;
            }
            destDir = dir;
            StorageAccess.setDestPath(this, dir);
            refreshStorage();
        });
    }

    interface Take {
        void dir(File f);
    }

    /** A plain folder browser, only reachable with All Files Access. Never runs on a URI. */
    void browse(File start, Take take) {
        File dir = StorageAccess.readable(start) ? start : StorageAccess.sharedRoot();
        showBrowser(dir, take);
    }

    void showBrowser(File dir, Take take) {
        List<File> dirs = StorageAccess.dirs(dir);
        final String[] names = new String[dirs.size() + 1];
        names[0] = getString(R.string.browse_here);
        for (int i = 0; i < dirs.size(); i++) names[i + 1] = dirs.get(i).getName();
        new AlertDialog.Builder(this)
                .setTitle(dir.getAbsolutePath())
                .setItems(names, (d, which) -> {
                    if (which == 0) {
                        take.dir(dir);
                        return;
                    }
                    File next = dirs.get(which - 1);
                    if (dir.getParentFile() == null) return; /* never climb above the volume */
                    showBrowser(next, take);
                })
                .setNeutralButton(R.string.browse_up, (d, w) -> {
                    File up = dir.getParentFile();
                    if (up != null && StorageAccess.readable(up)) showBrowser(up, take);
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    /* ---- install ---- */

    void install() {
        if (!canInstall()) return;
        busy = true;
        isoButton.setEnabled(false);
        zipButton.setEnabled(false);
        installButton.setEnabled(false);
        storageButton.setEnabled(false);
        sourceButton.setEnabled(false);
        destButton.setEnabled(false);
        bar.setVisibility(View.VISIBLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        new Thread(() -> {
            try {
                /* Everything below works on real paths. Prove each one is usable before touching it. */
                File base = destDir != null ? destDir : root;
                if (destDir != null && !StorageAccess.inAppStorage(this, destDir)
                        && !StorageAccess.allFiles(this))
                    throw new IOException(getString(R.string.dest_needs_access));
                if (!base.isDirectory() && !base.mkdirs())
                    throw new IOException(getString(R.string.dest_unwritable, base.getAbsolutePath()));
                if (!StorageAccess.readable(base))
                    throw new IOException(getString(R.string.dest_unwritable, base.getAbsolutePath()));
                if (StorageAccess.freeBytes(base) < NEED_BYTES)
                    throw new IOException(getString(R.string.dest_no_space));
                if (sourceDir != null && !StorageAccess.looksInstalled(sourceDir))
                    throw new IOException(getString(R.string.source_not_install, sourceDir.getAbsolutePath()));
                if (sourceDir == null && sourceTree == null && iso == null)
                    throw new IOException(getString(R.string.install_bad));
                if (sourceTree != null && !(StorageAccess.granted(this, sourceTree)
                        && StorageAccess.listable(this, sourceTree)))
                    throw new IOException(getString(R.string.source_tree_lost));

                say(getString(R.string.checking_exe));
                buildImage();

                File data2 = new File(base, "data2");
                if (sourceDir != null || sourceTree != null) {
                    copyGame(data2);
                } else {
                    unpackDisc(data2);
                }
                /* The one thing worth insisting on: the tree the game looks for is really there. */
                say(getString(R.string.verifying));
                if (!new File(data2, "Resource").isDirectory())
                    throw new IOException(getString(R.string.install_bad));
                writeGameRoot(base, data2);
                try (FileOutputStream o = new FileOutputStream(new File(root, "install.ok"))) { o.getFD().sync(); }
                ui.post(() -> { busy = false; Installer a = live; if (a != null) a.launch(); });
            } catch (Throwable e) { /* an Error (OutOfMemoryError) must not kill the thread silently */
                ui.post(() -> {
                    busy = false;
                    Installer a = live;
                    if (a == null) return;
                    a.bar.setVisibility(View.GONE);
                    a.status.setText("Setup failed: " + e.getMessage());
                    a.isoButton.setEnabled(true);
                    a.zipButton.setEnabled(true);
                    a.storageButton.setEnabled(true);
                    a.sourceButton.setEnabled(true);
                    a.destButton.setEnabled(true);
                    a.installButton.setEnabled(a.canInstall());
                    a.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                });
            }
        }).start();
    }

    /* The disc image path: unshield unpacks the cabinets straight out of the picked descriptor. */
    void unpackDisc(File data2) throws Exception {
        ui.post(() -> bar.setIndeterminate(false));
        Runnable[] tick = new Runnable[1];
        tick[0] = () -> {
            int permille = progress();
            bar.setProgress(permille);
            status.setText(getString(R.string.unpacking) + permille / 10 + "%");
            ui.postDelayed(tick[0], 500);
        };
        ui.post(tick[0]);
        int rc;
        try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(iso, "r")) {
            if (pfd == null) throw new IOException("That disc image cannot be opened.");
            rc = extractIso(pfd.getFd(), data2.getPath());
        } finally {
            ui.removeCallbacks(tick[0]);
        }
        if (rc == -1) throw new IOException("That file is not a disc image (.iso).");
        if (rc == -2) throw new IOException("No Virtua Tennis 3 PC install files on that disc image.");
        if (rc < 0) throw new IOException("Unpacking failed. Check there is about 4.5 GB free.");
    }

    /* The folder path: in place when it is already a real path, through a copy when it came from
     * the picker. Either way this ends with a plain directory tree in real storage. */
    void copyGame(File data2) throws IOException {
        /* The bar is a view: the copy runs off the UI thread, so every touch of it is posted. */
        ui.post(() -> {
            bar.setProgress(0);
            bar.setIndeterminate(true);
        });
        if (sourceDir != null) {
            copyDir(sourceDir, data2, 0, new long[] { 0 });
        } else {
            StorageAccess.copyTree(this, sourceTree, data2, (bytes, name) ->
                    say(getString(R.string.copying) + (bytes >> 20) + " MB"));
        }
        ui.post(() -> bar.setIndeterminate(false));
    }

    static void copyDir(File src, File dst, int depth, long[] bytes) throws IOException {
        if (depth > MAX_DEPTH) throw new IOException("That folder is nested too deeply.");
        File[] all = src.listFiles();
        if (all == null) throw new IOException("Cannot read " + src.getName());
        if (!dst.isDirectory() && !dst.mkdirs()) throw new IOException("Cannot create " + dst);
        for (File f : all) {
            File out = new File(dst, f.getName());
            if (f.isDirectory()) {
                copyDir(f, out, depth + 1, bytes);
            } else if (f.isFile()) {
                try (InputStream in = new FileInputStream(f); OutputStream o = new FileOutputStream(out)) {
                    byte[] b = new byte[1 << 16];
                    for (int n; (n = in.read(b)) > 0; ) {
                        o.write(b, 0, n);
                        bytes[0] += n;
                    }
                }
            }
            /* Anything else (a fifo, a socket, a device node) is not game data: skip it. */
        }
    }

    /* The runtime reads the image from app storage and the data from game_root, which is only
     * written when the player chose another folder. */
    void writeGameRoot(File base, File data2) throws IOException {
        File marker = new File(root, "game_root");
        if (destDir == null) {
            if (marker.exists() && !marker.delete()) throw new IOException("Cannot clear " + marker);
            return;
        }
        try (FileOutputStream o = new FileOutputStream(marker)) {
            o.write(data2.getAbsolutePath().getBytes());
            o.getFD().sync();
        }
    }

    /* VT3.exe may be picked bare or inside a zip. */
    void buildImage() throws Exception {
        byte[] exe;
        try (InputStream in = getContentResolver().openInputStream(zip)) {
            if (in == null) throw new IOException("That VT3.exe cannot be opened.");
            exe = readAll(in);
        }
        if (exe.length > 1 && exe[0] == 'P' && exe[1] == 'K') {
            byte[] found = null;
            try (ZipInputStream z = new ZipInputStream(new java.io.ByteArrayInputStream(exe))) {
                for (ZipEntry e; (e = z.getNextEntry()) != null; )
                    if (e.getName().toLowerCase().endsWith("vt3.exe")) { found = readAll(z); break; }
            }
            if (found == null) throw new IOException("No VT3.exe in that zip.");
            exe = found;
        }
        byte[] img = flattenPe(exe);
        if (img == null || !IMAGE_SHA256.equals(hex(MessageDigest.getInstance("SHA-256").digest(img))))
            throw new IOException("That VT3.exe is not the v1.0.1 build this port needs.");
        File tmp = new File(root, "image.bin.tmp");
        try (FileOutputStream o = new FileOutputStream(tmp)) { o.write(img); o.getFD().sync(); }
        if (!tmp.renameTo(new File(root, "image.bin"))) throw new IOException("Could not write image.bin.");
    }

    /* Through a .tmp and a rename, so a full disk never leaves a truncated dst. */
    void copyAsset(String name, File dst) throws IOException {
        File tmp = new File(dst.getPath() + ".tmp");
        try (InputStream in = getAssets().open(name); FileOutputStream o = new FileOutputStream(tmp)) {
            byte[] b = new byte[1 << 16];
            for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n);
            o.getFD().sync();
        }
        if (!tmp.renameTo(dst)) throw new IOException("Could not write " + dst.getName());
    }

    void launch() {
        /* An update can ship a new shader pack (it is keyed to the shader translator), so refresh it. */
        try {
            long stamp = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
            File mark = new File(root, "shaders.stamp");
            String want = Long.toString(stamp);
            if (!mark.exists() || !want.equals(new String(readAll(new java.io.FileInputStream(mark))))) {
                copyAsset("shaders.pack", new File(root, "shaders.pack"));
                try (OutputStream o = new FileOutputStream(mark)) { o.write(want.getBytes()); }
            }
        } catch (Exception ignored) {
        }
        /* Resolve the graphics ladder (device tier, preset, per-option overrides, thermal rung)
         * into the markers the renderer reads, and keep the thermal status current while it runs. */
        GraphicsProfile.apply(this, root);
        ThermalWatch.start(this);
        startActivity(new Intent(this, NativeActivity.class));
        finish();
    }

    static void say(String s) { ui.post(() -> { Installer a = live; if (a != null) a.status.setText(s); }); }

    /* Capped: VT3.exe is a few MB, and picking the 4.5 GB disc image here would run out of memory. */
    static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] b = new byte[1 << 16];
        for (int n; (n = in.read(b)) > 0; ) {
            if (o.size() + n > 64 << 20) { in.close(); throw new IOException("That file is too big to be VT3.exe (over 64 MB)."); }
            o.write(b, 0, n);
        }
        in.close();
        return o.toByteArray();
    }

    static String hex(byte[] d) {
        StringBuilder s = new StringBuilder();
        for (byte x : d) s.append(String.format("%02x", x));
        return s.toString();
    }
}
