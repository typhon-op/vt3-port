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
 * Launcher. On first run the player picks their PC disc image, an already-unpacked game folder, or
 * both, plus the v1.0.1 VT3.exe; this builds image.bin in the app's files dir (the VT3_ANDROID_ROOT
 * the game reads) and records where the game data lives, then writes install.ok. Every later launch
 * goes straight to the game.
 *
 * Two install modes, decided by {@link InstallPlan}:
 *
 *   folder, writable   read where it lies: nothing is copied, and {@code <root>/game_root} tells the
 *                      runtime the data root (it reads VT3_GAME_ROOT, see android_entry.c and
 *                      tools/recomp/rt/vt3rt.c). The folder must be writable because the game writes
 *                      its vt3.ini and its saves there.
 *   folder, read-only  copied into app storage instead, with the reason on the screen.
 *   picker tree        copied: a content:// tree has no POSIX path for the native side to open,
 *                      unless it maps to a readable real path, in which case it is used in place.
 *   disc image         unpacked, unchanged.
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

    File root;
    final Source source = new Source();
    File destDir, sourceDir;
    Button isoButton, zipButton, installButton, cancelButton, storageButton, sourceButton,
            destButton, diagnosticsButton;
    TextView isoState, exeState, status, storageState, sourceState, destState;
    ProgressBar bar;
    static final Handler ui = new Handler(Looper.getMainLooper());
    /* The install thread outlives the activity (a recreate, or a relaunch from the icon), so it
     * reports to whichever instance is on screen, and a second install never runs over the first. */
    static volatile Installer live;
    static volatile boolean busy;
    static volatile boolean cancelRequested;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        root = getExternalFilesDir(null);
        DiagLog.i("install", "setup screen opened; data root " + root);
        boolean lost = false;
        if (new File(root, "install.ok").exists()) {
            File data = installedDataRoot();
            if (data != null && GameFolder.isDataRoot(data)) {
                DiagLog.i("install", "already installed; starting the game");
                DiagLog.i("storage", "all files access " + (StorageAccess.allFiles(this) ? "granted" : "not granted"));
                launch();
                return;
            }
            /* The marker survived but the data behind it did not (a folder moved, a card removed).
             * Show setup instead of launching into a failed start, and keep everything else. */
            DiagLog.w("install", "install.ok is present but " + data + " is not a readable data root");
            lost = true;
        }
        setContentView(R.layout.activity_installer);
        isoButton = findViewById(R.id.iso_button);
        zipButton = findViewById(R.id.exe_button);
        installButton = findViewById(R.id.install_button);
        cancelButton = findViewById(R.id.cancel_button);
        isoState = findViewById(R.id.iso_state);
        exeState = findViewById(R.id.exe_state);
        status = findViewById(R.id.status);
        bar = findViewById(R.id.progress);
        storageButton = findViewById(R.id.storage_button);
        sourceButton = findViewById(R.id.source_button);
        destButton = findViewById(R.id.dest_button);
        diagnosticsButton = findViewById(R.id.diagnostics_button);
        storageState = findViewById(R.id.storage_state);
        sourceState = findViewById(R.id.source_state);
        destState = findViewById(R.id.dest_state);
        isoButton.setOnClickListener(v -> pick(PICK_ISO));
        zipButton.setOnClickListener(v -> pick(PICK_ZIP));
        installButton.setOnClickListener(v -> install());
        cancelButton.setOnClickListener(v -> cancel());
        storageButton.setOnClickListener(v -> askAllFiles());
        sourceButton.setOnClickListener(v -> pickSource());
        destButton.setOnClickListener(v -> pickDestination());
        diagnosticsButton.setOnClickListener(v -> startActivity(new Intent(this, Diagnostics.class)));
        /* A remembered folder can be gone: unmounted card, app data cleared, folder deleted. */
        sourceDir = StorageAccess.sourcePath(this);
        if (sourceDir != null && !GameFolder.isDataRoot(sourceDir)) {
            StorageAccess.setSourcePath(this, null);
            sourceDir = null;
        }
        if (sourceDir != null) {
            source.folder(sourceDir.getAbsolutePath());
            File exe = GameFolder.exeNear(sourceDir);
            if (exe != null) source.foundExe(exe.getAbsolutePath());
        }
        destDir = StorageAccess.destPath(this);
        live = this;
        if (busy) {
            isoButton.setEnabled(false);
            zipButton.setEnabled(false);
            status.setText(R.string.setup_running);
            showCancel(true);
        }
        refreshStorage();
        if (lost) status.setText(R.string.install_lost);
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
        if (!busy) {
            super.onBackPressed();
            return;
        }
        /* Leaving mid-install would leave a half-written tree behind: cancel it properly instead. */
        cancel();
    }

    void pick(int code) {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), code);
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        if (result != RESULT_OK || data == null) return;
        if (code == PICK_ISO) {
            /* The last pick wins: the folder is forgotten so it cannot override this image. */
            source.iso(data.getData().toString());
            sourceDir = null;
            StorageAccess.setSourcePath(this, null);
            isoButton.setText(R.string.step1_done);
            isoState.setText(R.string.step1_done);
            isoState.setTextColor(getColor(R.color.vt3_ball));
        }
        if (code == PICK_ZIP) {
            source.pickedExe(data.getData().toString());
            zipButton.setText(R.string.step2_done);
            exeState.setText(R.string.step2_done);
            exeState.setTextColor(getColor(R.color.vt3_ball));
        }
        if (code == PICK_TREE) takeTree(data);
        refreshStorage();
    }

    /* ---- the folder routes ---- */

    /**
     * A tree from the picker. A real path the app may also write is read in place; any other tree
     * that validates is copied; anything else is refused with the reason, and the player is asked
     * to pick again.
     */
    void takeTree(Intent data) {
        Uri tree = data.getData();
        if (tree == null) return;
        /* Not every provider persists grants; that must not reject an otherwise usable folder. */
        StorageAccess.remember(this, tree, data);
        if (!StorageAccess.listable(this, tree)) {
            DiagLog.w("storage", "picked tree " + tree + " is not listable with the grant we were given");
            StorageAccess.forget(this, tree);
            status.setText(R.string.source_tree_failed);
            return;
        }
        /* One resolution, whichever route follows: the real folder when the tree has a readable
         * path, the picker's listing otherwise. Both go through GameFolder's rule. */
        File real = StorageAccess.treePath(tree);
        GameFolder.Result r = real != null ? GameFolder.of(real)
                : GameFolder.resolve(new StorageAccess.SafListing(this, tree));
        if (!r.ok()) {
            DiagLog.i("storage", "picked tree " + tree + " refused: " + GameFolder.problemText(r));
            StorageAccess.forget(this, tree);
            status.setText(real != null ? problemText(real, r) : problemText(tree, r));
            return;
        }
        File dataRoot = GameFolder.root(real, r);
        /* A real path the app may also write is read where it lies. Anything else is copied: the
         * game writes vt3.ini and its saves, and a content:// tree has no path to open. */
        if (real != null && TreeFiles.writable(dataRoot)) {
            selectFolder(real, r);
            DiagLog.i("storage", "tree " + tree + " is " + dataRoot + ": read in place");
            return;
        }
        source.tree(tree.toString(), r.root);
        sourceDir = null;
        StorageAccess.setSourcePath(this, null);
        status.setText(real != null
                ? getString(R.string.source_tree_read_only, dataRoot.getAbsolutePath())
                : getString(R.string.source_tree_ready));
        DiagLog.i("storage", "tree " + tree + " accepted for copying, data folder '"
                + (r.root.isEmpty() ? "." : r.root) + "'"
                + (real == null ? "" : ", " + dataRoot + " being read-only"));
    }

    void selectFolder(File pickedDir, GameFolder.Result r) {
        File dataRoot = GameFolder.root(pickedDir, r);
        source.folder(dataRoot.getAbsolutePath());
        sourceDir = dataRoot;
        StorageAccess.setSourcePath(this, dataRoot);
        File exe = GameFolder.exe(pickedDir, r);
        if (exe == null) exe = GameFolder.exeNear(dataRoot);
        if (exe != null) {
            source.foundExe(exe.getAbsolutePath());
            zipButton.setText(R.string.step2_done);
            exeState.setText(getString(R.string.exe_found, exe.getName()));
            exeState.setTextColor(getColor(R.color.vt3_ball));
        }
        /* Said as the plan will do it: a folder the app may not write is copied, not read in
         * place, and claiming otherwise here is what made the old space figure look wrong. */
        status.setText(TreeFiles.writable(dataRoot)
                ? getString(R.string.source_using, dataRoot.getAbsolutePath())
                : getString(R.string.source_read_only, dataRoot.getAbsolutePath()));
    }

    /* ---- storage UI ---- */

    void refreshStorage() {
        if (storageButton == null || busy) return;
        boolean all = StorageAccess.allFiles(this);
        storageState.setText(all ? R.string.storage_granted
                : StorageAccess.asked(this) ? R.string.storage_denied_asked : R.string.storage_denied);
        storageState.setTextColor(getColor(all ? R.color.vt3_ball : R.color.vt3_muted));
        storageButton.setText(all ? R.string.storage_check : R.string.storage_allow);
        if (source.folder != null) {
            File f = new File(source.folder);
            if (!GameFolder.readable(f)) {
                sourceState.setText(getString(R.string.source_missing, source.folder));
            } else if (TreeFiles.writable(f)) {
                sourceState.setText(getString(R.string.source_using, source.folder));
            } else {
                /* Said now, not after a 4.5 GB copy has already started. */
                sourceState.setText(getString(R.string.source_read_only, source.folder));
            }
        } else if (source.treeUri != null) {
            if (StorageAccess.listable(this, Uri.parse(source.treeUri))) {
                sourceState.setText(R.string.source_tree);
            } else {
                /* Revoked or deleted since it was picked: say so instead of failing at the end. */
                StorageAccess.forget(this, Uri.parse(source.treeUri));
                source.treeUri = null;
                source.treeChild = "";
                sourceState.setText(R.string.source_tree_lost);
                status.setText(R.string.source_tree_lost);
            }
        } else {
            sourceState.setText(R.string.source_disc);
        }
        /* A folder that validated but has no VT3.exe anywhere near it: say why Install stays off. */
        if (!source.hasExe() && (source.folder != null || source.treeUri != null))
            exeState.setText(R.string.exe_needed);
        destState.setText(destDir == null ? getString(R.string.dest_default) : destDir.getAbsolutePath());
        installButton.setEnabled(canInstall());
    }

    /** The button follows the plan the current choice would run, not the ISO/EXE steps. */
    boolean canInstall() {
        return !busy && InstallPlan.readyToInstall(source, folderWritable(), copyDest());
    }

    /** A chosen folder the app may also write, which is what makes the plan direct. */
    boolean folderWritable() {
        return source.folder != null && TreeFiles.writable(new File(source.folder));
    }

    String copyDest() {
        return new File(destDir != null ? destDir : root, "data2").getAbsolutePath();
    }

    /** The plan the screen is showing; the install recomputes it with the real byte counts. */
    InstallPlan screenPlan() {
        return InstallPlan.forSource(source, folderWritable(), copyDest());
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
        DiagLog.i("storage", "opening the all-files access settings screen");
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
            browse(StorageAccess.sharedRoot(), this::takeFolder);
            return;
        }
        /* No broad access: the picker's own grant is enough, and the copy lands in real storage. */
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_TREE);
        } catch (Throwable e) {
            status.setText(R.string.storage_no_picker);
        }
    }

    /** A folder from the in-app browser: validated with the same rule as a picked tree. */
    void takeFolder(File dir) {
        GameFolder.Result r = GameFolder.of(dir);
        if (!r.ok()) {
            DiagLog.i("storage", "folder " + dir + " refused: " + GameFolder.problemText(r));
            status.setText(problemText(dir, r));
            return;
        }
        selectFolder(dir, r);
        refreshStorage();
    }

    void pickDestination() {
        if (busy) return;
        if (!StorageAccess.allFiles(this)) {
            /* Said, not silently ignored; the settings button beside it is the way forward. */
            status.setText(R.string.dest_needs_access);
            return;
        }
        browse(StorageAccess.sharedRoot(), dir -> {
            long need = gateBytes();
            if (StorageAccess.freeBytes(dir) < need) {
                status.setText(getString(R.string.dest_no_space, mb(need)));
                return;
            }
            destDir = dir;
            StorageAccess.setDestPath(this, dir);
            refreshStorage();
        });
    }

    /**
     * What the next install will need at its destination, taken from its plan: a folder read in
     * place asks for the image headroom only, never the unpack of the disc image. Only a folder
     * that has to be copied is measured, since that is the only case where its size matters.
     */
    long gateBytes() {
        InstallPlan plan = screenPlan();
        if (plan == null) return InstallPlan.UNPACK_BYTES;
        if (plan.mode == InstallPlan.Mode.COPY_FOLDER)
            return InstallPlan.copyNeed(TreeFiles.bytes(new File(source.folder)));
        return plan.needFreeBytes;
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

    void showCancel(boolean on) {
        if (cancelButton == null) return;
        cancelButton.setVisibility(on ? View.VISIBLE : View.GONE);
        cancelButton.setEnabled(on);
    }

    /** Asks the running install to stop; it removes what it wrote, never the player's folder. */
    void cancel() {
        if (!busy) return;
        cancelRequested = true;
        status.setText(R.string.cancelling);
        showCancel(false);
        DiagLog.i("install", "cancel requested");
    }

    void install() {
        if (!canInstall()) return;
        final String folder = source.folder, tree = source.treeUri, child = source.treeChild,
                iso = source.isoUri, exeUri = source.exeUri, exePath = source.exePath;
        busy = true;
        cancelRequested = false;
        isoButton.setEnabled(false);
        zipButton.setEnabled(false);
        installButton.setEnabled(false);
        storageButton.setEnabled(false);
        sourceButton.setEnabled(false);
        destButton.setEnabled(false);
        showCancel(true);
        bar.setVisibility(View.VISIBLE);
        bar.setIndeterminate(true);
        bar.setProgress(0);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        DiagLog.i("install", "install started; destination "
                + (destDir != null ? destDir.getAbsolutePath() : root.getAbsolutePath())
                + "; source " + source.describe()
                + "; exe " + source.describeExe()
                + "; all files access " + (StorageAccess.allFiles(this) ? "granted" : "not granted"));
        new Thread(() -> run(folder, tree, child, iso, exeUri, exePath)).start();
    }

    void run(String folder, String treeUri, String child, String isoUri, String exeUri, String exePath) {
        File created = null;
        try {
            File selected = folder == null ? null : new File(folder);
            File copyBase = destDir != null ? destDir : root;
            File data2 = new File(copyBase, "data2");

            /* Everything below works on real paths. Prove each one is usable before touching it. */
            if (selected != null) {
                if (!GameFolder.isDataRoot(selected))
                    throw new IOException(getString(R.string.source_not_install,
                            selected.getAbsolutePath()));
                if (!StorageAccess.inAppStorage(this, selected) && !StorageAccess.allFiles(this))
                    throw new IOException(getString(R.string.source_needs_access));
            }
            if (destDir != null && !StorageAccess.inAppStorage(this, destDir)
                    && !StorageAccess.allFiles(this))
                throw new IOException(getString(R.string.dest_needs_access));
            if (!root.isDirectory() && !root.mkdirs() && !root.isDirectory())
                throw new IOException(getString(R.string.dest_unwritable, root.getAbsolutePath()));

            /* Being able to write the folder is what decides direct mode, so a folder that is read
             * in place is not measured: counting a 4.5 GB tree that nothing copies is wasted work. */
            boolean writable = selected != null && TreeFiles.writable(selected);
            long sourceBytes = 0;
            if (selected != null && !writable) sourceBytes = TreeFiles.bytes(selected);
            InstallPlan plan;
            if (selected != null) {
                plan = InstallPlan.folder(selected.getAbsolutePath(), writable, sourceBytes,
                        data2.getAbsolutePath());
            } else if (treeUri != null) {
                if (!StorageAccess.listable(this, Uri.parse(treeUri)))
                    throw new IOException(getString(R.string.source_tree_lost));
                plan = InstallPlan.tree(data2.getAbsolutePath());
            } else if (isoUri != null) {
                plan = InstallPlan.iso(data2.getAbsolutePath());
            } else {
                throw new IOException(getString(R.string.install_no_source));
            }
            DiagLog.i("install", "plan: " + plan.describe());

            /* The destination the operation actually writes to, and the space it actually needs. */
            File base = plan.copies() ? copyBase : root;
            if (plan.copies() && !base.isDirectory() && !base.mkdirs() && !base.isDirectory())
                throw new IOException(getString(R.string.dest_unwritable, base.getAbsolutePath()));
            if (!StorageAccess.readable(base))
                throw new IOException(getString(R.string.dest_unwritable, base.getAbsolutePath()));
            long free = StorageAccess.freeBytes(base);
            if (free < plan.needFreeBytes)
                throw new IOException(getString(R.string.install_no_space,
                        mb(plan.needFreeBytes), mb(free)));

            say(getString(R.string.checking_exe));
            buildImage(exeUri, exePath);

            switch (plan.mode) {
                case DIRECT:
                    /* Nothing is copied: the game reads the folder the player chose. */
                    DiagLog.i("install", "direct mode: game data stays at " + plan.gameRoot
                            + ", nothing copied");
                    break;
                case COPY_FOLDER:
                    if (inside(selected, data2))
                        throw new IOException(getString(R.string.dest_inside_source));
                    if (data2.exists() && !TreeFiles.delete(data2))
                        throw new IOException(getString(R.string.install_cleanup_failed, data2.getAbsolutePath()));
                    created = data2;
                    say(getString(R.string.copying) + "0 MB");
                    TreeFiles.copy(selected, data2, sourceBytes, copyProgress(), () -> cancelRequested);
                    break;
                case COPY_TREE:
                    if (data2.exists() && !TreeFiles.delete(data2))
                        throw new IOException(getString(R.string.install_cleanup_failed, data2.getAbsolutePath()));
                    created = data2;
                    say(getString(R.string.copying) + "0 MB");
                    StorageAccess.copyTree(this, Uri.parse(treeUri), child, data2, copyProgress(),
                            () -> cancelRequested);
                    break;
                case UNPACK:
                    created = data2;
                    unpackDisc(data2, isoUri);
                    break;
                default:
                    throw new IOException(getString(R.string.install_bad));
            }

            /* The one thing worth insisting on: the tree the game will read is really there. */
            say(getString(R.string.verifying));
            File dataRoot = new File(plan.gameRoot);
            if (!GameFolder.isDataRoot(dataRoot))
                throw new IOException(getString(R.string.install_incomplete,
                        dataRoot.getAbsolutePath()));
            writeGameRoot(plan.gameRoot);
            DiagLog.i("install", "game data complete at " + plan.gameRoot
                    + (plan.direct() ? " (read in place; nothing copied)" : ""));
            try (FileOutputStream o = new FileOutputStream(new File(root, "install.ok"))) {
                o.getFD().sync();
            }
            ui.post(() -> {
                busy = false;
                showCancel(false);
                Installer a = live;
                if (a != null) a.launch();
            });
        } catch (Throwable e) { /* an Error (OutOfMemoryError) must not kill the thread silently */
            /* A half-copied tree must not survive: the next launch would read a mix of two sources. */
            boolean cleaned = created == null || TreeFiles.delete(created);
            DiagLog.e("install", 0, "install failed" + (cleaned ? "" : " (partial data left behind)"), e);
            final String message = cancelRequested
                    ? getString(R.string.install_cancelled)
                    : "Setup failed: " + e.getMessage();
            ui.post(() -> {
                busy = false;
                Installer a = live;
                if (a == null) return;
                a.bar.setVisibility(View.GONE);
                a.status.setText(message);
                a.isoButton.setEnabled(true);
                a.zipButton.setEnabled(true);
                a.storageButton.setEnabled(true);
                a.sourceButton.setEnabled(true);
                a.destButton.setEnabled(true);
                a.installButton.setEnabled(a.canInstall());
                a.showCancel(false);
                a.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            });
        }
    }

    /** Byte progress for the foot strip: determinate when the total is known. */
    TreeFiles.Progress copyProgress() {
        return (bytes, total) -> {
            final int permille = total > 0 ? (int) Math.min(1000, bytes * 1000 / total) : 0;
            final boolean known = total > 0;
            final String text = known
                    ? getString(R.string.copying_progress, mb(bytes), mb(total))
                    : getString(R.string.copying) + mb(bytes);
            ui.post(() -> {
                Installer a = live;
                if (a == null) return;
                a.bar.setIndeterminate(!known);
                if (known) a.bar.setProgress(permille);
                a.status.setText(text);
            });
        };
    }

    /* The disc image path: unshield unpacks the cabinets straight out of the picked descriptor. */
    void unpackDisc(File data2, String isoUri) throws Exception {
        ui.post(() -> bar.setIndeterminate(false));
        Runnable[] tick = new Runnable[1];
        tick[0] = () -> {
            if (!busy || cancelRequested) return;
            int permille = progress();
            bar.setProgress(permille);
            status.setText(getString(R.string.unpacking) + permille / 10 + "%");
            ui.postDelayed(tick[0], 500);
        };
        ui.post(tick[0]);
        int rc;
        try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(Uri.parse(isoUri), "r")) {
            if (pfd == null) throw new IOException("That disc image cannot be opened.");
            rc = extractIso(pfd.getFd(), data2.getPath());
        } finally {
            ui.removeCallbacks(tick[0]);
        }
        if (cancelRequested) throw new IOException("Install cancelled.");
        if (rc == -1) throw new IOException("That file is not a disc image (.iso).");
        if (rc == -2) throw new IOException("No Virtua Tennis 3 PC install files on that disc image.");
        if (rc < 0) throw new IOException("Unpacking failed. Check there is about 4.5 GB free.");
    }

    /**
     * The runtime reads the image from app storage and the data from game_root, which is written
     * whenever the data root is not the default <root>/data2 -- a chosen install folder, or the
     * player's own folder read in place.
     */
    void writeGameRoot(String gameRoot) throws IOException {
        File marker = new File(root, "game_root");
        File def = new File(root, "data2");
        if (gameRoot == null || same(gameRoot, def.getAbsolutePath())) {
            if (marker.exists() && !marker.delete()) throw new IOException("Cannot clear " + marker);
            return;
        }
        try (FileOutputStream o = new FileOutputStream(marker)) {
            o.write(gameRoot.getBytes());
            o.getFD().sync();
        }
    }

    /** The data root an installed game reads, from the marker or the default. */
    File installedDataRoot() {
        File marker = new File(root, "game_root");
        if (marker.isFile()) {
            try (InputStream in = new FileInputStream(marker)) {
                String path = new String(readAll(in)).trim();
                if (!path.isEmpty()) return new File(path);
            } catch (IOException | RuntimeException e) {
                DiagLog.w("install", "cannot read " + marker + ": " + e);
            }
        }
        return new File(root, "data2");
    }

    /* VT3.exe may be picked bare or inside a zip, or found in the game folder itself. */
    void buildImage(String exeUri, String exePath) throws Exception {
        byte[] exe;
        if (exePath != null) {
            File f = new File(exePath);
            if (!f.isFile()) throw new IOException(getString(R.string.exe_missing, exePath));
            try (InputStream in = new FileInputStream(f)) {
                exe = readAll(in);
            }
        } else if (exeUri != null) {
            try (InputStream in = getContentResolver().openInputStream(Uri.parse(exeUri))) {
                if (in == null) throw new IOException("That VT3.exe cannot be opened.");
                exe = readAll(in);
            }
        } else {
            throw new IOException(getString(R.string.install_no_exe));
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
            byte[] b = new byte[TreeFiles.CHUNK];
            for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n);
            o.getFD().sync();
        }
        if (!tmp.renameTo(dst)) throw new IOException("Could not write " + dst.getName());
    }

    void launch() {
        DiagLog.i("app", "launching the game");
        /* An update can ship a new shader pack (it is keyed to the shader translator), so refresh it. */
        try {
            long stamp = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
            File mark = new File(root, "shaders.stamp");
            String want = Long.toString(stamp);
            if (!mark.exists() || !want.equals(new String(readAll(new FileInputStream(mark))))) {
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

    /* ---- messages and small helpers ---- */

    String problemText(File dir, GameFolder.Result r) {
        String path = dir.getAbsolutePath();
        if (GameFolder.NO_RESOURCE.equals(r.problem))
            return getString(R.string.source_not_install, path);
        if (GameFolder.AMBIGUOUS.equals(r.problem))
            return getString(R.string.source_ambiguous, path, r.detail);
        return getString(R.string.source_unreadable, path);
    }

    String problemText(Uri tree, GameFolder.Result r) {
        if (GameFolder.NO_RESOURCE.equals(r.problem)) return getString(R.string.source_tree_no_resource);
        if (GameFolder.AMBIGUOUS.equals(r.problem)) return getString(R.string.source_tree_ambiguous, r.detail);
        return getString(R.string.source_tree_failed);
    }

    static void say(String s) {
        ui.post(() -> {
            Installer a = live;
            if (a != null) a.status.setText(s);
        });
    }

    static String mb(long bytes) {
        long m = Math.round(bytes / (double) (1 << 20));
        return m >= 1024 ? String.format(java.util.Locale.US, "%.1f GB", m / 1024.0) : m + " MB";
    }

    static boolean same(String a, String b) {
        try {
            return new File(a).getCanonicalPath().equals(new File(b).getCanonicalPath());
        } catch (IOException e) {
            return new File(a).getAbsolutePath().equals(new File(b).getAbsolutePath());
        }
    }

    /** True when child is parent or inside it: the install must never write into the game folder. */
    static boolean inside(File parent, File child) {
        if (parent == null || child == null) return false;
        try {
            String p = parent.getCanonicalPath();
            String c = child.getCanonicalPath();
            return c.equals(p) || c.startsWith(p + File.separator);
        } catch (IOException e) {
            return child.getAbsolutePath().startsWith(parent.getAbsolutePath() + File.separator);
        }
    }

    /* Capped: VT3.exe is a few MB, and picking the 4.5 GB disc image here would run out of memory. */
    static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] b = new byte[TreeFiles.CHUNK];
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
