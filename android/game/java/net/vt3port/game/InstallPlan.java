package net.vt3port.game;

/**
 * What an install will actually do, decided before a single file is touched: read the game where it
 * lies, copy a folder, copy a picked tree, or unpack the disc.
 *
 * The policy that matters is here, in one place, so it can be tested without a device: a folder is
 * used in place when it is writable, and copied only when it is not. The runtime can read an
 * external data root (it is told through {@code VT3_GAME_ROOT}), but it also writes there -- vt3.ini
 * for its widescreen frame, the game's own saves -- so a folder it may read but not write is not a
 * usable data root and is copied instead.
 *
 * Pure Java: no Android imports, exercised by tools/installtest/InstallHostTest.java.
 */
final class InstallPlan {
    enum Mode {
        /** Read the selected folder where it lies: nothing is copied. */
        DIRECT,
        /** Copy the selected real folder into app storage. */
        COPY_FOLDER,
        /** Copy a SAF tree into app storage (the picker's tree has no POSIX path). */
        COPY_TREE,
        /** Unpack the disc image into app storage. */
        UNPACK
    }

    /** Headroom over the copied bytes: the image, the shader pack and log rotation live here too. */
    static final long SLACK = 64L << 20;
    /** What an unpacked disc costs, with headroom: the documented ~4.5 GB plus the above. */
    static final long UNPACK_BYTES = 5L << 30;

    final Mode mode;
    /** Absolute path of the data root the native side will read. */
    final String gameRoot;
    /** Free bytes the destination needs before this operation is allowed to start. */
    final long needFreeBytes;
    /** Why this mode was chosen, for the log. */
    final String reason;

    private InstallPlan(Mode mode, String gameRoot, long needFreeBytes, String reason) {
        this.mode = mode;
        this.gameRoot = gameRoot;
        this.needFreeBytes = needFreeBytes;
        this.reason = reason;
    }

    boolean copies() {
        return mode != Mode.DIRECT;
    }

    boolean direct() {
        return mode == Mode.DIRECT;
    }

    String describe() {
        return mode + " -> " + gameRoot + (reason == null ? "" : " (" + reason + ")");
    }

    /**
     * A selected real folder: read in place when the app may write there, otherwise copied.
     * {@code selectedRoot} is already the data root (the folder that holds Resource).
     */
    static InstallPlan folder(String selectedRoot, boolean writable, long sourceBytes, String copyDest) {
        if (writable)
            return new InstallPlan(Mode.DIRECT, selectedRoot, SLACK,
                    "read in place, nothing copied");
        return new InstallPlan(Mode.COPY_FOLDER, copyDest, copyNeed(sourceBytes),
                "read-only folder, copied into app storage");
    }

    /**
     * Free space a copy of a source of this size needs: what it holds plus the headroom. An
     * unmeasurable source gets the unpack gate, which is the largest install this port can need.
     */
    static long copyNeed(long sourceBytes) {
        return sourceBytes > 0 ? sourceBytes + SLACK : UNPACK_BYTES;
    }

    /**
     * The plan the setup screen is looking at, or null when nothing is chosen. No I/O and no byte
     * scan: the screen needs the mode and a gate, and a read-only folder's real size is measured
     * once, by the install itself. The install recomputes this with the measured size.
     */
    static InstallPlan forSource(Source s, boolean writable, String copyDest) {
        if (s.folder != null) return folder(s.folder, writable, 0, copyDest);
        if (s.treeUri != null) return tree(copyDest);
        if (s.isoUri != null) return iso(copyDest);
        return null;
    }

    /**
     * Whether Install may be pressed: a source with a plan, and the VT3.exe that image.bin is built
     * from. A valid unpacked folder needs neither a disc image nor a separately picked executable
     * once its own VT3.exe has been found -- or, for a read-only folder or a picked tree, once the
     * exe has been found in it.
     */
    static boolean readyToInstall(Source s, boolean writable, String copyDest) {
        return forSource(s, writable, copyDest) != null && s.hasExe();
    }

    /** A SAF tree: the native side cannot open a content:// URI, so it is copied. */
    static InstallPlan tree(String dataRoot) {
        return new InstallPlan(Mode.COPY_TREE, dataRoot, UNPACK_BYTES,
                "picked through the file picker, copied into app storage");
    }

    /** The disc image: unpacked into app storage. */
    static InstallPlan iso(String dataRoot) {
        return new InstallPlan(Mode.UNPACK, dataRoot, UNPACK_BYTES,
                "unpacked from the disc image");
    }
}
