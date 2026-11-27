package net.vt3port.game;

/**
 * The one game source the setup screen currently has chosen, and the VT3.exe that goes with it.
 *
 * Choosing disc image clears the folder, and choosing a folder clears the disc image: the install
 * always uses the last thing the player picked, never a remembered earlier one. Pure Java so the
 * host test can check the switching without an Android runtime.
 */
final class Source {
    /** The picked disc image, as a content:// URI string. */
    String isoUri;
    /** The picked folder tree, as a content:// URI string (the copy route). */
    String treeUri;
    /** Inside the tree, the child that holds Resource ("" when the tree root does). */
    String treeChild = "";
    /** The data root itself, as a real path (the direct route). */
    String folder;
    /** A VT3.exe the player picked, as a content:// URI string (bare or a .zip). */
    String exeUri;
    /** A VT3.exe found inside the chosen folder, as a real path. */
    String exePath;

    void iso(String uri) {
        isoUri = uri;
        folder = null;
        treeUri = null;
        treeChild = "";
    }

    void tree(String uri, String child) {
        treeUri = uri;
        treeChild = child == null ? "" : child;
        isoUri = null;
        folder = null;
    }

    void folder(String path) {
        folder = path;
        isoUri = null;
        treeUri = null;
        treeChild = "";
    }

    void pickedExe(String uri) {
        exeUri = uri;
        exePath = null;
    }

    void foundExe(String path) {
        exePath = path;
        exeUri = null;
    }

    boolean hasGame() {
        return folder != null || treeUri != null || isoUri != null;
    }

    boolean hasExe() {
        return exeUri != null || exePath != null;
    }

    /** What the install log calls the source: the one that will actually be used. */
    String describe() {
        if (folder != null) return folder;
        if (treeUri != null) return treeUri + (treeChild.isEmpty() ? "" : "/" + treeChild);
        if (isoUri != null) return isoUri;
        return "none";
    }

    String describeExe() {
        if (exePath != null) return exePath;
        if (exeUri != null) return exeUri;
        return "none";
    }
}
