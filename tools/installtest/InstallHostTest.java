package net.vt3port.game;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/**
 * Host test for the unpacked-folder route, with no Android runtime:
 *
 *   javac -d /tmp/vt3install android/game/java/net/vt3port/game/GameFolder.java \
 *         android/game/java/net/vt3port/game/InstallPlan.java \
 *         android/game/java/net/vt3port/game/Source.java \
 *         android/game/java/net/vt3port/game/TreeFiles.java tools/installtest/InstallHostTest.java
 *   java -cp /tmp/vt3install net.vt3port.game.InstallHostTest
 *
 * It lives in the app's package so it can see the package-private installer classes, and it imports
 * nothing from android.*: only java.io, java.nio.file and java.util.
 *
 * What it establishes: both accepted folder levels, case-insensitive Resource and VT3.exe
 * detection, the refusal messages, the same rule over a SAF-style listing, direct mode being chosen
 * with the selected folder as the native game root, the copy fallback for a read-only folder, the
 * space the copy will need, folder/disc switching, and that a failed or cancelled copy removes what
 * it wrote and leaves the source untouched. None of it launches the game: that needs a device.
 */
public final class InstallHostTest {
    private static int checks;
    private static final List<String> failures = new ArrayList<>();

    public static void main(String[] args) throws Exception {
        File tmp = Files.createTempDirectory("vt3install").toFile();

        gameRootAccepted(tmp);
        parentWithOneChildAccepted(tmp);
        ambiguousParentRefused(tmp);
        missingResourceRefused(tmp);
        unreadableFolderRefused(tmp);
        caseInsensitiveNames(tmp);
        execFound(tmp);
        safListingUsesTheSameRule();
        directModePlan(tmp);
        copyFallbackPlan(tmp);
        spaceGate();
        selectionSwitching();
        copySucceeds(tmp);
        failedCopyCleansUp(tmp);
        cancelledCopyCleansUp(tmp);
        treeBytesDoesNotFollowLinks(tmp);

        TreeFiles.delete(tmp);
        System.out.println();
        if (!failures.isEmpty()) {
            System.out.println("InstallHostTest: " + failures.size() + " FAILED");
            for (String f : failures) System.out.println("  FAILED  " + f);
            System.exit(1);
        }
        System.out.println("InstallHostTest: " + checks + " checks passed");
    }

    /* ---- the folder rule ---- */

    static void gameRootAccepted(File tmp) throws IOException {
        File root = gameInstall(tmp, "vt3-pc");
        GameFolder.Result r = GameFolder.of(root);
        check(r.ok(), "a folder holding Resource is accepted");
        check(r.root.isEmpty(), "the folder itself is the data root");
        check("Resource".equals(r.resource), "the Resource name is reported");
        check(GameFolder.isDataRoot(root), "isDataRoot() agrees");
        check(GameFolder.root(root, r).equals(root), "the data root is the picked folder");
    }

    static void parentWithOneChildAccepted(File tmp) throws IOException {
        File install = new File(tmp, "vt3-pc-root");
        File data = new File(install, "data2");
        Files.createDirectories(new File(data, "Resource").toPath());
        touch(new File(install, "VT3.exe"));
        GameFolder.Result r = GameFolder.of(install);
        check(r.ok(), "a parent with one child holding Resource is accepted");
        check("data2".equals(r.root), "the single child is the data root (" + r.root + ")");
        check(GameFolder.root(install, r).equals(data), "the data root resolves to <picked>/data2");
        check("VT3.exe".equals(r.exe), "VT3.exe in the picked folder is found");
        check(GameFolder.exe(install, r) != null, "the executable file resolves");
    }

    static void ambiguousParentRefused(File tmp) throws IOException {
        File parent = new File(tmp, "ambiguous");
        Files.createDirectories(new File(parent, "data1/Resource").toPath());
        Files.createDirectories(new File(parent, "data2/Resource").toPath());
        GameFolder.Result r = GameFolder.of(parent);
        check(!r.ok(), "two children holding Resource is refused, not guessed");
        check(GameFolder.AMBIGUOUS.equals(r.problem), "the reason is AMBIGUOUS (" + r.problem + ")");
        check(r.detail != null && r.detail.contains("data1") && r.detail.contains("data2"),
                "both candidates are named in the message (" + r.detail + ")");
    }

    static void missingResourceRefused(File tmp) throws IOException {
        File empty = new File(tmp, "empty");
        Files.createDirectories(empty.toPath());
        check(GameFolder.NO_RESOURCE.equals(GameFolder.of(empty).problem), "an empty folder has no Resource");

        File files = new File(tmp, "only-files");
        Files.createDirectories(files.toPath());
        touch(new File(files, "VT3.exe"));
        touch(new File(files, "readme.txt"));
        check(GameFolder.NO_RESOURCE.equals(GameFolder.of(files).problem),
                "a folder with files but no Resource is refused");

        File data1 = new File(tmp, "data1-only");
        Files.createDirectories(new File(data1, "Resource").toPath());
        Files.createDirectories(new File(data1, "other").toPath());
        File notADir = new File(tmp, "resource-is-a-file");
        Files.createDirectories(notADir.toPath());
        touch(new File(notADir, "Resource"));
        check(GameFolder.NO_RESOURCE.equals(GameFolder.of(notADir).problem),
                "a Resource that is a file is not a data root");
    }

    static void unreadableFolderRefused(File tmp) throws IOException {
        File gone = new File(tmp, "not-there");
        GameFolder.Result r = GameFolder.of(gone);
        check(!r.ok() && GameFolder.UNREADABLE.equals(r.problem), "a missing folder reports UNREADABLE");
    }

    static void caseInsensitiveNames(File tmp) throws IOException {
        File lower = new File(tmp, "lower");
        Files.createDirectories(new File(lower, "resource").toPath());
        touch(new File(lower, "vt3.exe"));
        GameFolder.Result r = GameFolder.of(lower);
        check(r.ok(), "a lower-case 'resource' folder is accepted");
        check("resource".equals(r.resource), "the on-disk name is kept as it is (" + r.resource + ")");
        check("vt3.exe".equals(r.exe), "VT3.exe is matched ignoring case (" + r.exe + ")");

        File child = new File(tmp, "lower-parent");
        Files.createDirectories(new File(child, "DATA2/RESOURCE").toPath());
        GameFolder.Result c = GameFolder.of(child);
        check(c.ok() && "DATA2".equals(c.root), "the child rule ignores case too (" + c.root + ")");
    }

    static void execFound(File tmp) throws IOException {
        File install = new File(tmp, "exe-beside");
        File data = new File(install, "data2");
        Files.createDirectories(new File(data, "Resource").toPath());
        touch(new File(data, "VT3.exe"));
        GameFolder.Result r = GameFolder.of(data);
        check("VT3.exe".equals(r.exe) && r.exeWhere.isEmpty(),
                "VT3.exe beside Resource is found in the data folder");
        check(GameFolder.exe(data, r) != null, "it resolves to a file");

        File noExe = new File(tmp, "exe-none");
        Files.createDirectories(new File(noExe, "Resource").toPath());
        GameFolder.Result n = GameFolder.of(noExe);
        check(n.ok() && n.exe == null, "a folder without VT3.exe still validates");
        check(GameFolder.exeNear(noExe) == null, "no executable is invented");
        check(GameFolder.exeNear(data) != null, "exeNear finds the one that is there");
    }

    /** The same rule must work over a folder listing that is not a File, as the SAF route is. */
    static void safListingUsesTheSameRule() {
        GameFolder.Result root = GameFolder.resolve(new FakeListing()
                .dir("", "data2").dir("", "Resource"));
        check(root.ok() && root.root.isEmpty(), "a SAF tree root holding Resource is accepted");

        GameFolder.Result wrapped = GameFolder.resolve(new FakeListing()
                .dir("", "data2").dir("", "VT3.exe").dir("data2", "Resource"));
        check(wrapped.ok() && "data2".equals(wrapped.root),
                "a SAF tree with one data child resolves to it (" + wrapped.root + ")");
        check("VT3.exe".equals(wrapped.exe), "the executable in the tree root is found");

        GameFolder.Result bad = GameFolder.resolve(new FakeListing().dir("", "data1").dir("", "data2"));
        check(GameFolder.NO_RESOURCE.equals(bad.problem), "a SAF tree without Resource is refused");

        GameFolder.Result gone = GameFolder.resolve(new GameFolder.Listing() {
            public String[] names(String path) { return null; }
            public boolean isDir(String path, String name) { return false; }
        });
        check(GameFolder.UNREADABLE.equals(gone.problem), "an unlistable SAF tree reports UNREADABLE");
    }

    /* ---- the install plan ---- */

    static void directModePlan(File tmp) throws IOException {
        File root = gameInstall(tmp, "direct");
        File dest = new File(tmp, "app-storage-direct");
        InstallPlan p = InstallPlan.folder(root.getAbsolutePath(), true,
                TreeFiles.bytes(root), new File(dest, "data2").getAbsolutePath());
        check(p.mode == InstallPlan.Mode.DIRECT, "a writable folder is read in place");
        check(!p.copies(), "direct mode copies nothing");
        check(new File(p.gameRoot).equals(root),
                "the native game root is the selected folder (" + p.gameRoot + ")");
        check(new File(p.gameRoot, "Resource").isDirectory(),
                "the game root really holds Resource, so the runtime can open it");
        check(!new File(dest, "data2").exists(), "nothing was created under the install destination");
        check(p.needFreeBytes == InstallPlan.SLACK,
                "direct mode only asks for the image and shader headroom");
    }

    static void copyFallbackPlan(File tmp) throws IOException {
        File root = gameInstall(tmp, "read-only");
        long bytes = TreeFiles.bytes(root);
        File dest = new File(tmp, "app-storage-copy");
        InstallPlan p = InstallPlan.folder(root.getAbsolutePath(), false, bytes,
                new File(dest, "data2").getAbsolutePath());
        check(p.mode == InstallPlan.Mode.COPY_FOLDER, "a read-only folder falls back to a copy");
        check(p.copies(), "the fallback plan copies");
        check(p.gameRoot.equals(new File(dest, "data2").getAbsolutePath()),
                "the native game root becomes the copied data2 (" + p.gameRoot + ")");
        check(p.needFreeBytes == bytes + InstallPlan.SLACK,
                "the space gate is the source size plus headroom (" + p.needFreeBytes + ")");
        check(p.reason.contains("read-only"), "the reason is recorded (" + p.reason + ")");

        InstallPlan tree = InstallPlan.tree("/data/app/data2");
        check(tree.mode == InstallPlan.Mode.COPY_TREE && tree.copies(),
                "a SAF tree is copied: the native side cannot open a content:// URI");
        InstallPlan iso = InstallPlan.iso("/data/app/data2");
        check(iso.mode == InstallPlan.Mode.UNPACK, "the disc image still unpacks");
        check(iso.needFreeBytes == InstallPlan.UNPACK_BYTES, "the unpack gate is unchanged");
    }

    static void spaceGate() {
        check(InstallPlan.copyNeed(4L << 30) == (4L << 30) + InstallPlan.SLACK,
                "a 4 GB folder needs 4 GB plus headroom");
        check(InstallPlan.copyNeed(0) == InstallPlan.UNPACK_BYTES,
                "an unknown size asks for the conservative gate");
        check(InstallPlan.copyNeed(100L << 30) == (100L << 30) + InstallPlan.SLACK,
                "a huge source is gated at its real size, so the copy is refused before it starts");
    }

    /* ---- selection: the last pick wins ---- */

    static void selectionSwitching() {
        Source s = new Source();
        check(!s.hasGame(), "nothing is chosen at first");

        s.folder("/storage/emulated/0/VT3/data2");
        check(s.folder != null, "a folder can be the source");
        s.iso("content://disc.iso");
        check(s.folder == null, "picking the disc image clears the remembered folder");
        check(s.isoUri != null, "the disc image is the source now");

        s.folder("/storage/emulated/0/VT3/data2");
        check(s.isoUri == null, "picking a folder clears the disc image");
        check(s.treeUri == null, "and any picked tree");

        s.tree("content://tree", "data2");
        check(s.folder == null && s.isoUri == null, "picking a tree clears both others");
        check("data2".equals(s.treeChild), "the tree's data child is kept");
        check(s.describe().endsWith("/data2"), "the log names the tree's data folder");

        s.iso("content://disc.iso");
        check(s.treeUri == null && "".equals(s.treeChild), "the disc image clears the tree child");

        s.pickedExe("content://VT3.exe");
        check(s.hasExe(), "a picked executable counts");
        s.foundExe("/storage/emulated/0/VT3/VT3.exe");
        check(s.exeUri == null && s.exePath != null, "a found executable replaces the picked one");
        check(s.describeExe().endsWith("VT3.exe"), "the log names the executable");
    }

    /* ---- copy, failure and cancellation ---- */

    static void copySucceeds(File tmp) throws IOException {
        File src = gameInstall(tmp, "copy-src");
        File dst = new File(tmp, "copy-dst");
        long total = TreeFiles.bytes(src);
        long[] last = { 0 };
        long copied = TreeFiles.copy(src, dst, total, (bytes, t) -> last[0] = bytes, () -> false);
        check(copied == total, "the copy moved every byte (" + copied + " of " + total + ")");
        check(last[0] == total, "progress reached the total");
        check(new File(dst, "Resource").isDirectory(), "the copy keeps the Resource tree");
        check(new File(dst, "Resource/court.dat").isFile(), "file contents are copied, not just names");
        check(new File(src, "Resource/court.dat").isFile(), "the source is left untouched");
    }

    static void failedCopyCleansUp(File tmp) throws IOException {
        File src = new File(tmp, "fail-src");
        Files.createDirectories(new File(src, "Resource").toPath());
        touch(new File(src, "Resource/big.dat"));
        Files.createDirectories(new File(src, "sub").toPath());
        touch(new File(src, "sub/deep.dat"));
        File dst = new File(tmp, "fail-dst");
        /* A file where the copy needs a directory: mkdirs() fails halfway through. */
        Files.createDirectories(dst.toPath());
        touch(new File(dst, "sub"));
        boolean threw = false;
        try {
            TreeFiles.copy(src, dst, TreeFiles.bytes(src), null, () -> false);
        } catch (IOException e) {
            threw = true;
        }
        check(threw, "a copy that cannot create a directory fails loudly");
        check(!dst.exists(), "the partial copy is removed");
        check(new File(src, "Resource/big.dat").isFile(), "the source folder survives a failed copy");
    }

    static void cancelledCopyCleansUp(File tmp) throws IOException {
        File src = gameInstall(tmp, "cancel-src");
        File dst = new File(tmp, "cancel-dst");
        boolean threw = false;
        try {
            TreeFiles.copy(src, dst, TreeFiles.bytes(src), null, () -> true);
        } catch (IOException e) {
            threw = true;
            check(e.getMessage().contains("cancelled"), "the failure says it was cancelled");
        }
        check(threw, "a cancelled copy stops");
        check(!dst.exists(), "a cancelled copy leaves nothing behind");
        check(new File(src, "Resource/court.dat").isFile(), "the source is untouched by a cancel");
    }

    static void treeBytesDoesNotFollowLinks(File tmp) throws IOException {
        File dir = new File(tmp, "links");
        Files.createDirectories(new File(dir, "Resource").toPath());
        touch(new File(dir, "Resource/a.dat"));
        long plain = TreeFiles.bytes(dir);
        check(plain > 0, "bytes() counts file content (" + plain + ")");
        try {
            Files.createSymbolicLink(new File(dir, "loop").toPath(), dir.toPath());
        } catch (IOException | UnsupportedOperationException e) {
            System.out.println("  --  symlinks not available here; the loop check was skipped");
            return;
        }
        check(TreeFiles.bytes(dir) == plain, "bytes() does not walk a symlink loop");
        long[] seen = { 0 };
        File dst = new File(tmp, "links-copy");
        TreeFiles.copy(dir, dst, plain, (b, t) -> seen[0] = b, () -> false);
        check(!new File(dst, "loop").exists(), "a copy does not follow the link either");
    }

    /* ---- helpers ---- */

    static File gameInstall(File tmp, String name) throws IOException {
        File dir = new File(tmp, name);
        Files.createDirectories(new File(dir, "Resource").toPath());
        touch(new File(dir, "Resource/court.dat"));
        touch(new File(dir, "VT3.exe"));
        return dir;
    }

    static void touch(File f) throws IOException {
        Files.createDirectories(f.getParentFile().toPath());
        try (FileOutputStream o = new FileOutputStream(f)) {
            o.write("vt3".getBytes(StandardCharsets.UTF_8));
        }
    }

    static void check(boolean ok, String what) {
        checks++;
        if (ok) {
            System.out.println("  ok  " + what);
        } else {
            System.out.println("  FAIL " + what);
            failures.add(what);
        }
    }

    /** A folder listing built by hand, standing in for a SAF tree. */
    static final class FakeListing implements GameFolder.Listing {
        private final List<String[]> dirs = new ArrayList<>();   /* {parent, name} for dirs */
        private final List<String[]> files = new ArrayList<>();  /* {parent, name} for files */

        FakeListing dir(String parent, String name) {
            dirs.add(new String[] { parent, name });
            return this;
        }

        FakeListing file(String parent, String name) {
            files.add(new String[] { parent, name });
            return this;
        }

        public String[] names(String path) {
            List<String> out = new ArrayList<>();
            for (String[] d : dirs) if (d[0].equals(path)) out.add(d[1]);
            for (String[] f : files) if (f[0].equals(path)) out.add(f[1]);
            return out.toArray(new String[0]);
        }

        public boolean isDir(String path, String name) {
            for (String[] d : dirs) if (d[0].equals(path) && d[1].equals(name)) return true;
            return false;
        }
    }
}
