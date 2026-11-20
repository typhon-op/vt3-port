package net.vt3port.game;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.RandomAccessFile;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

/**
 * The log file engine: one bounded queue, one writer thread, rotating plain-text files.
 *
 * Deliberately free of android.* imports so the same class can be exercised by the host tests.
 * Producers only offer an entry and return; when the queue is full the entry is dropped and
 * counted. The writer thread owns every file operation, so a UI or game thread never waits on
 * storage, and losing the writer (a crash, a kill) can only lose the queue, never block anyone.
 *
 * Files: {@code vt3.log} (current), {@code vt3.1.log} … {@code vt3.<keep>.log} (older). When the
 * current file would pass {@code limit} it is rotated and the oldest is deleted, so the directory
 * cannot grow past roughly {@code limit * (keep + 1)}.
 */
public final class DiagCore {
    public static final int DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3, FATAL = 4;
    public static final String[] LEVELS = { "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };

    /** Bounded: a stalled writer loses detail but never applies backpressure to a frame. */
    public static final int QUEUE_CAPACITY = 1024;

    private static final class Entry {
        final long millis;
        final int level;
        final String component;
        final int code;
        final String message;
        final String stack;

        Entry(long millis, int level, String component, int code, String message, String stack) {
            this.millis = millis;
            this.level = level;
            this.component = component;
            this.code = code;
            this.message = message;
            this.stack = stack;
        }
    }

    private final File dir;
    private final String base;
    private final int keep;
    private final Object fileLock = new Object();
    private final BlockingQueue<Entry> queue = new ArrayBlockingQueue<>(QUEUE_CAPACITY);
    private final AtomicLong dropped = new AtomicLong();
    private final AtomicLong accepted = new AtomicLong();
    private final Thread writer;
    private final SimpleDateFormat stamp =
            new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US);

    private volatile long limit;
    private volatile boolean running = true;
    private Writer out;
    private long size;
    private String lastError;

    public DiagCore(File dir, String base, long limit, int keep) {
        this.dir = dir;
        this.base = base;
        this.limit = Math.max(1024, limit);
        this.keep = Math.max(1, keep);
        if (!dir.isDirectory()) dir.mkdirs();
        this.writer = new Thread(this::run, "vt3-diag");
        this.writer.setDaemon(true);
        this.lastError = readSmall(new File(dir, "last_error.txt"));
        synchronized (fileLock) {
            size = currentFile().length();
        }
        this.writer.start();
    }

    /* ---- producing ---------------------------------------------------------- */

    public void log(int level, String component, int code, String message) {
        log(level, component, code, message, null);
    }

    public void log(int level, String component, int code, String message, Throwable error) {
        if (message == null) message = "";
        String stack = error == null ? null : stackOf(error);
        Entry e = new Entry(System.currentTimeMillis(), level, component == null ? "app" : component,
                code, message, stack);
        if (!queue.offer(e)) {
            dropped.incrementAndGet();
            return;
        }
        accepted.incrementAndGet();
    }

    public long dropped() { return dropped.get(); }

    public long accepted() { return accepted.get(); }

    /** The size of every log file this engine owns. */
    public long sizeBytes() {
        synchronized (fileLock) {
            long total = 0;
            for (File f : logFiles()) total += f.length();
            return total;
        }
    }

    public File directory() { return dir; }

    public String lastError() {
        return lastError == null ? "" : lastError;
    }

    public void setLimit(long bytes) {
        long v = Math.max(1024, bytes);
        limit = v;
    }

    /* ---- session markers ----------------------------------------------------- */

    /**
     * Starts a session. Returns the previous session's start time when its marker was left behind,
     * which means the app was killed or crashed rather than shut down: the caller records that.
     */
    public String beginSession() {
        File marker = new File(dir, "session.txt");
        String previous = marker.exists() ? readSmall(marker) : null;
        writeSmall(marker, new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date()));
        log(INFO, "app", 0, "session start");
        return previous;
    }

    public void endSession() {
        log(INFO, "app", 0, "session end");
        flush(1500);
        new File(dir, "session.txt").delete();
    }

    /* ---- reading ------------------------------------------------------------- */

    /** The most recent {@code maxBytes} of the log, oldest line first, as one string. */
    public String tail(int maxBytes) {
        synchronized (fileLock) {
            flushLocked();
            List<File> files = logFiles();     /* current first */
            List<String> chunks = new ArrayList<>();
            long left = Math.max(1024, maxBytes);
            for (File f : files) {
                if (left <= 0) break;
                long len = f.length();
                long take = Math.min(len, left);
                if (take <= 0) continue;
                chunks.add(readTailOf(f, (int) take));
                left -= take;
            }
            StringBuilder sb = new StringBuilder();
            for (int i = chunks.size() - 1; i >= 0; i--) sb.append(chunks.get(i));
            return sb.toString();
        }
    }

    /** Lines at WARN or above from the tail, newest last. Used by the report's error history. */
    public List<String> errors(int maxBytes) {
        String[] lines = tail(maxBytes).split("\n");
        List<String> out = new ArrayList<>();
        for (String line : lines) {
            if (line.contains("  WARN ") || line.contains("  ERROR ") || line.contains("  FATAL "))
                out.add(line);
        }
        return out;
    }

    /** Lines from one component (the lifecycle section of the report), newest last. */
    public List<String> linesOf(String component, int maxBytes) {
        String[] lines = tail(maxBytes).split("\n");
        List<String> out = new ArrayList<>();
        if (component == null) return out;
        for (String line : lines) {
            if (line.contains(" " + component + " ")) out.add(line);
        }
        return out;
    }

    /* ---- maintenance --------------------------------------------------------- */

    /** Empties the queue and deletes the log files, last-error record and either-side crash file. */
    public void clear() {
        queue.clear();
        synchronized (fileLock) {
            closeLocked();
            for (File f : logFiles()) f.delete();
            size = 0;
            out = null;
            new File(dir, "last_error.txt").delete();
            new File(dir, "native_crash.txt").delete();
            lastError = "";
        }
    }

    public void flush(long timeoutMs) {
        long until = System.currentTimeMillis() + timeoutMs;
        synchronized (fileLock) {
            while (!queue.isEmpty() && System.currentTimeMillis() < until) {
                try {
                    fileLock.wait(20);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
            flushLocked();
        }
    }

    /** Stops the writer. Anything still queued is written first. */
    public void close() {
        running = false;
        writer.interrupt();
        try {
            writer.join(3000);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        flush(0);
    }

    /* ---- writer thread ------------------------------------------------------- */

    private void run() {
        List<Entry> batch = new ArrayList<>(128);
        while (running || !queue.isEmpty()) {
            Entry first;
            try {
                first = queue.poll(200, TimeUnit.MILLISECONDS);
            } catch (InterruptedException e) {
                break;
            }
            if (first == null) {
                synchronized (fileLock) {
                    flushLocked();
                }
                continue;
            }
            batch.clear();
            batch.add(first);
            queue.drainTo(batch, 127);
            synchronized (fileLock) {
                boolean failed = false;
                for (Entry e : batch) {
                    try {
                        writeLocked(e);
                    } catch (IOException | RuntimeException io) {
                        /* One bad write must not kill the writer: retry with a fresh handle. */
                        failed = true;
                    }
                }
                if (failed) closeLocked();
                flushLocked();
                fileLock.notifyAll();
            }
        }
        synchronized (fileLock) {
            flushLocked();
            closeLocked();
        }
    }

    /* ---- file primitives ----------------------------------------------------- */

    private File currentFile() { return new File(dir, base + ".log"); }

    /** Current log first, then rotations, newest to oldest. */
    private List<File> logFiles() {
        List<File> files = new ArrayList<>();
        File cur = currentFile();
        if (cur.exists()) files.add(cur);
        for (int i = 1; i <= keep; i++) {
            File f = new File(dir, base + "." + i + ".log");
            if (f.exists()) files.add(f);
        }
        return files;
    }

    private void openLocked() throws IOException {
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("no log directory");
        out = new OutputStreamWriter(new FileOutputStream(currentFile(), true), StandardCharsets.UTF_8);
        size = currentFile().length();
    }

    private void closeLocked() {
        if (out != null) {
            try {
                out.close();
            } catch (IOException ignored) {
            }
            out = null;
        }
    }

    private void flushLocked() {
        if (out != null) {
            try {
                out.flush();
            } catch (IOException ignored) {
            }
        }
    }

    private void writeLocked(Entry e) throws IOException {
        String line = format(e);
        if (out == null) openLocked();
        if (size > 0 && size + line.length() > limit) {
            rotateLocked();
            if (out == null) openLocked();   /* rotation closes the old file */
        }
        out.write(line);
        size += line.length();
        if (e.level >= ERROR) rememberError(e);
    }

    private void rotateLocked() {
        closeLocked();
        new File(dir, base + "." + keep + ".log").delete();
        for (int i = keep - 1; i >= 1; i--) {
            File from = new File(dir, base + "." + i + ".log");
            if (from.exists()) from.renameTo(new File(dir, base + "." + (i + 1) + ".log"));
        }
        File cur = currentFile();
        if (cur.exists()) cur.renameTo(new File(dir, base + ".1.log"));
        size = 0;
        out = null;
    }

    private String format(Entry e) {
        StringBuilder sb = new StringBuilder(160);
        synchronized (stamp) {
            sb.append(stamp.format(new Date(e.millis)));
        }
        sb.append("  ").append(pad(LEVELS[Math.max(0, Math.min(LEVELS.length - 1, e.level))], 5))
                .append("  ").append(pad(e.component, 10));
        if (e.code != 0) sb.append("  code=").append(e.code);
        sb.append("  ").append(e.message.replace('\n', ' ')).append('\n');
        if (e.stack != null && !e.stack.isEmpty()) {
            for (String l : e.stack.split("\n")) sb.append("        ").append(l).append('\n');
        }
        return sb.toString();
    }

    private static String pad(String s, int width) {
        if (s == null) s = "";
        if (s.length() >= width) return s;
        StringBuilder sb = new StringBuilder(s);
        while (sb.length() < width) sb.append(' ');
        return sb.toString();
    }

    /** The last error of this session, and the record the next launch can show. */
    private void rememberError(Entry e) {
        String text = format(e).trim();
        lastError = text;    /* the newest error is the one the report and the next launch show */
        File f = new File(dir, "last_error.txt");
        File tmp = new File(dir, "last_error.txt.tmp");
        try (Writer w = new OutputStreamWriter(new FileOutputStream(tmp), StandardCharsets.UTF_8)) {
            w.write(text);
            w.write('\n');
        } catch (IOException ignored) {
            return;
        }
        if (!tmp.renameTo(f)) tmp.delete();
    }

    private static String stackOf(Throwable t) {
        StringBuilder sb = new StringBuilder();
        sb.append(t.toString()).append('\n');
        for (StackTraceElement el : t.getStackTrace()) sb.append("at ").append(el).append('\n');
        Throwable cause = t.getCause();
        if (cause != null && cause != t) {
            sb.append("caused by ").append(cause).append('\n');
            for (StackTraceElement el : cause.getStackTrace()) sb.append("at ").append(el).append('\n');
        }
        return sb.toString();
    }

    private static String readSmall(File f) {
        if (!f.isFile() || f.length() > 64 * 1024) return f.isFile() ? "" : null;
        try (InputStreamReader r = new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8)) {
            StringBuilder sb = new StringBuilder();
            char[] b = new char[512];
            for (int n; (n = r.read(b)) > 0; ) sb.append(b, 0, n);
            return sb.toString().trim();
        } catch (IOException e) {
            return null;
        }
    }

    private static void writeSmall(File f, String text) {
        try (Writer w = new OutputStreamWriter(new FileOutputStream(f), StandardCharsets.UTF_8)) {
            w.write(text);
        } catch (IOException ignored) {
        }
    }

    private static String readTailOf(File f, int maxBytes) {
        try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {
            long len = raf.length();
            long start = Math.max(0, len - maxBytes);
            byte[] buf = new byte[(int) (len - start)];
            raf.seek(start);
            raf.readFully(buf);
            String s = new String(buf, StandardCharsets.UTF_8);
            if (start > 0) {
                int nl = s.indexOf('\n');
                if (nl >= 0) s = s.substring(nl + 1);
            }
            return s;
        } catch (IOException e) {
            return "";
        }
    }
}
