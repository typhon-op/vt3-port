package net.vt3port.game;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * What counts as an unpacked Virtua Tennis 3 folder, and where inside a picked folder the data root
 * really is.
 *
 * Two folder levels are accepted, because a PC install root and the data folder under it are both
 * natural things to pick:
 *
 *   <picked>/Resource              -> the picked folder is the data root
 *   <picked>/<one child>/Resource  -> that single child is the data root (e.g. data1/../data2)
 *
 * Names are matched case-insensitively, like the runtime resolves them (the original data was
 * written for a case-insensitive filesystem). More than one child holding a Resource folder is
 * refused instead of guessed, so a pick that would copy the wrong tree fails immediately.
 *
 * Pure Java with no Android imports: the same code runs on the phone and in the host test
 * (tools/installtest/InstallHostTest.java), so the picker routes cannot drift apart again.
 */
final class GameFolder {
    static final String RESOURCE = "Resource";
    static final String EXE = "VT3.exe";

    /* Why a folder was refused. Mapped to a setup-screen string by the caller. */
    static final String UNREADABLE = "unreadable";
    static final String NO_RESOURCE = "no-resource";
    static final String AMBIGUOUS = "ambiguous";

    /** A read-only view of a folder tree: an All Files path and a SAF tree both implement it. */
    interface Listing {
        /** Entry names in the folder at path "" or "<one child>"; null when it cannot be listed. */
        String[] names(String path);

        /** True when {@code name} inside path is a directory. */
        boolean isDir(String path, String name);
    }

    /** The outcome of looking at a picked folder. */
    static final class Result {
        /** "" when the picked folder is the data root, else the single child that is. */
        final String root;
        /** The on-disk name of the Resource folder ("Resource", "resource", ...) or null. */
        final String resource;
        /** The on-disk name of a VT3.exe found with the folder, or null. */
        final String exe;
        /** "" for the picked folder, the child name, or ".." for the picked folder's parent. */
        final String exeWhere;
        /** One of UNREADABLE / NO_RESOURCE / AMBIGUOUS, or null when the folder is usable. */
        final String problem;
        /** Extra detail for the message: the child names when ambiguous. */
        final String detail;

        Result(String root, String resource, String exe, String exeWhere,
               String problem, String detail) {
            this.root = root;
            this.resource = resource;
            this.exe = exe;
            this.exeWhere = exeWhere;
            this.problem = problem;
            this.detail = detail;
        }

        boolean ok() {
            return problem == null;
        }
    }

    private GameFolder() {}

    /** A folder the process can open and list right now. */
    static boolean readable(File dir) {
        if (dir == null || !dir.isDirectory() || !dir.canRead()) return false;
        String[] list = dir.list();
        return list != null;
    }

    /** The names in a folder, or null when it cannot be read. */
    private static String[] listOf(File dir) {
        return readable(dir) ? dir.list() : null;
    }

    /** A folder that directly holds the Resource tree: the data root the runtime reads. */
    static boolean isDataRoot(File dir) {
        return resourceOf(dir) != null;
    }

    private static String resourceOf(File dir) {
        String res = name(listOf(dir), RESOURCE);
        return res != null && new File(dir, res).isDirectory() ? res : null;
    }

    /** Look at a real folder: both accepted levels, and a VT3.exe beside it. */
    static Result of(File picked) {
        if (!readable(picked)) return new Result("", null, null, "", UNREADABLE, null);
        Result r = resolve(new Files(picked));
        if (!r.ok() || r.exe != null) return r;
        /* The data folder picked on its own: the executable sits in the install root above it. */
        File up = picked.getParentFile();
        String e = up == null ? null : name(listOf(up), EXE);
        return e == null ? r : new Result(r.root, r.resource, e, "..", null, null);
    }

    /** The shared rule, over any folder listing. */
    static Result resolve(Listing l) {
        String[] top;
        try {
            top = l.names("");
        } catch (Throwable e) {
            top = null;
        }
        if (top == null) return new Result("", null, null, "", UNREADABLE, null);

        String res = name(top, RESOURCE);
        if (res != null && l.isDir("", res))
            return new Result("", res, name(top, EXE), "", null, null);

        List<String> hits = new ArrayList<>();
        for (String child : sorted(top)) {
            if (child.isEmpty() || child.equals(".") || child.equals("..")) continue;
            if (!l.isDir("", child)) continue;
            String[] inside;
            try {
                inside = l.names(child);
            } catch (Throwable e) {
                inside = null;
            }
            String r = name(inside, RESOURCE);
            if (r != null && l.isDir(child, r)) hits.add(child);
        }
        if (hits.size() == 1) {
            String child = hits.get(0);
            String r = name(l.names(child), RESOURCE);
            String exe = name(top, EXE);
            String where = "";
            if (exe == null) {
                exe = name(l.names(child), EXE);
                where = child;
            }
            return new Result(child, r, exe, where, null, null);
        }
        if (hits.isEmpty()) return new Result("", null, null, "", NO_RESOURCE, null);
        return new Result("", null, null, "", AMBIGUOUS, joinNames(hits));
    }

    /** The data root for a result, relative to the folder that was picked. */
    static File root(File picked, Result r) {
        return join(picked, r.root);
    }

    /** The VT3.exe found with a picked folder, or null. */
    static File exe(File picked, Result r) {
        if (r.exe == null) return null;
        File dir = join(picked, r.exeWhere);
        File f = dir == null ? null : new File(dir, r.exe);
        return f != null && f.isFile() ? f : null;
    }

    /** A VT3.exe sitting in the data root, or in the install root above it, or null. */
    static File exeNear(File dataRoot) {
        if (dataRoot == null) return null;
        String here = name(listOf(dataRoot), EXE);
        if (here != null && new File(dataRoot, here).isFile()) return new File(dataRoot, here);
        File up = dataRoot.getParentFile();
        String above = up == null ? null : name(listOf(up), EXE);
        return above != null && new File(up, above).isFile() ? new File(up, above) : null;
    }

    /** Join a relative path onto a base folder; ".." is the base's parent. */
    static File join(File base, String rel) {
        if (rel == null || rel.isEmpty()) return base;
        if (rel.equals("..")) return base == null ? null : base.getParentFile();
        return base == null ? null : new File(base, rel);
    }

    /** The name in {@code names} that equals {@code wanted} ignoring case, or null. */
    static String name(String[] names, String wanted) {
        if (names == null || wanted == null) return null;
        for (String n : names) if (n != null && n.equalsIgnoreCase(wanted)) return n;
        return null;
    }

    static String problemText(Result r) {
        return r.problem == null ? "ok" : r.problem + (r.detail == null ? "" : " " + r.detail);
    }

    private static List<String> sorted(String[] names) {
        List<String> out = new ArrayList<>();
        if (names != null) for (String n : names) if (n != null) out.add(n);
        Collections.sort(out, String.CASE_INSENSITIVE_ORDER);
        return out;
    }

    private static String joinNames(List<String> names) {
        StringBuilder s = new StringBuilder();
        for (String n : names) {
            if (s.length() > 0) s.append(", ");
            s.append(n);
        }
        return s.toString();
    }

    /** The Listing view of a real directory. */
    static final class Files implements Listing {
        private final File base;

        Files(File base) {
            this.base = base;
        }

        private File dir(String path) {
            return GameFolder.join(base, path);
        }

        @Override
        public String[] names(String path) {
            return listOf(dir(path));
        }

        @Override
        public boolean isDir(String path, String name) {
            File d = dir(path);
            return d != null && name != null && new File(d, name).isDirectory();
        }
    }
}
