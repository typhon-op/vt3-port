package net.vt3port.game;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;

/**
 * Everything the diagnostic report says, as plain values. Collected from Android, from the app's
 * own files and from the native metrics snapshot; kept free of android.* so the report builder can
 * be tested on a host.
 */
public final class DiagData {
    /** Ordered label/value pairs, one section each. */
    public final LinkedHashMap<String, String> app = new LinkedHashMap<>();
    public final LinkedHashMap<String, String> install = new LinkedHashMap<>();
    public final LinkedHashMap<String, String> graphics = new LinkedHashMap<>();
    public final LinkedHashMap<String, String> audio = new LinkedHashMap<>();

    /** Formatted lifecycle lines, oldest first. */
    public final List<String> lifecycle = new ArrayList<>();
    /** Human-readable problems, most important first. Empty means none were detected. */
    public final List<String> problems = new ArrayList<>();
    /** Recent warnings/errors from the log, oldest first. */
    public final List<String> errors = new ArrayList<>();

    public String crashLog = "";
    public String previousSession = "";
    public String lastError = "";
    public long generatedAt;
    public long logBytes;
    public long droppedEntries;
}
