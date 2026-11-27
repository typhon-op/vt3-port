package net.vt3port.game;

import android.app.Activity;
import android.app.NativeActivity;
import android.content.Context;
import android.content.Intent;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * The installed game, as the screens see it: where its data root is, whether it is still readable,
 * and how to start it. One place, so the setup screen and the main menu cannot disagree about what
 * "installed" means.
 *
 * Installed means all three of these, checked every time a screen needs to know:
 *   install.ok              the marker the setup screen writes after a verified install
 *   a data root with Resource/   the folder the runtime will read (app storage or the player's own)
 *   image.bin               the guest image built from the player's VT3.exe
 */
final class Game {
    static final String INSTALLED = "install.ok";
    static final String GAME_ROOT = "game_root";

    private Game() {}

    static File root(Context c) {
        return Markers.root(c);
    }

    /** The data root recorded by setup, or the default <root>/data2. */
    static File dataRoot(Context c) {
        File root = root(c);
        File marker = new File(root, GAME_ROOT);
        if (marker.isFile()) {
            try (InputStream in = new FileInputStream(marker)) {
                byte[] b = new byte[1024];
                int n = in.read(b);
                String path = n > 0 ? new String(b, 0, n, "UTF-8").trim() : "";
                if (!path.isEmpty()) return new File(path);
            } catch (IOException | RuntimeException e) {
                DiagLog.w("install", "cannot read " + marker + ": " + e);
            }
        }
        return new File(root, "data2");
    }

    static boolean installed(Context c) {
        return problem(c) == null;
    }

    /**
     * Why the game cannot start, or null when it can. Written for the player, not the log: the main
     * menu shows it and offers setup again.
     */
    static String problem(Context c) {
        File root = root(c);
        if (!new File(root, INSTALLED).isFile())
            return c.getString(R.string.menu_not_installed);
        File data = dataRoot(c);
        if (!data.isDirectory() || !data.canRead())
            return c.getString(R.string.menu_data_missing, data.getAbsolutePath());
        if (!GameFolder.isDataRoot(data))
            return c.getString(R.string.menu_data_invalid, data.getAbsolutePath());
        if (!new File(root, "image.bin").isFile())
            return c.getString(R.string.menu_image_missing);
        return null;
    }

    /** The game files are gone or unusable: forget the install so setup runs again. */
    static void forget(Context c) {
        File root = root(c);
        new File(root, INSTALLED).delete();
        new File(root, GAME_ROOT).delete();
        DiagLog.i("install", "install record cleared; setup will run again");
    }

    /** Start the game: refresh the shader pack if the app was updated, then hand over to native. */
    static void launch(Activity a) {
        File root = root(a);
        DiagLog.i("app", "launching the game");
        /* An update can ship a new shader pack (it is keyed to the shader translator), so refresh it. */
        try {
            long stamp = a.getPackageManager().getPackageInfo(a.getPackageName(), 0).lastUpdateTime;
            File mark = new File(root, "shaders.stamp");
            String want = Long.toString(stamp);
            if (!mark.isFile() || !want.equals(Markers.read(root, "shaders.stamp", ""))) {
                copyAsset(a, "shaders.pack", new File(root, "shaders.pack"));
                try (OutputStream o = new FileOutputStream(mark)) { o.write(want.getBytes()); }
            }
        } catch (Exception ignored) {
        }
        /* Resolve the graphics ladder into the markers the renderer reads, and keep the thermal
         * status current while the game runs. */
        GraphicsProfile.apply(a, root);
        ThermalWatch.start(a);
        a.startActivity(new Intent(a, NativeActivity.class));
        a.finish();
    }

    /** Through a .tmp and a rename, so a full disk never leaves a truncated destination. */
    static void copyAsset(Context c, String name, File dst) throws IOException {
        File tmp = new File(dst.getPath() + ".tmp");
        try (InputStream in = c.getAssets().open(name); FileOutputStream o = new FileOutputStream(tmp)) {
            byte[] b = new byte[64 * 1024];
            for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n);
            o.getFD().sync();
        }
        if (!tmp.renameTo(dst)) throw new IOException("Could not write " + dst.getName());
    }
}
