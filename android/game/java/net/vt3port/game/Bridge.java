package net.vt3port.game;

import android.content.Context;
import android.content.Intent;

/**
 * The one entry point the native runtime calls into Java: the quick menu's Settings row.
 *
 * The native side has no Activity reference to work with, and an Application context can start an
 * activity with NEW_TASK, so that is all this needs. Every call is guarded: if the app has not
 * attached yet, the native menu simply stays open instead of crashing.
 */
public final class Bridge {
    private static volatile Context app;

    private Bridge() {}

    static void attach(Context c) {
        app = c.getApplicationContext();
    }

    /** Called from android_jni.c (cached as net/vt3port/game/Bridge.openTouchSettings). */
    static void openTouchSettings() {
        Context c = app;
        if (c == null) return;
        try {
            Intent i = new Intent(c, TouchControls.class);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            c.startActivity(i);
        } catch (RuntimeException e) {
            DiagLog.w("app", "could not open the touchscreen controls: " + e.getMessage());
        }
    }

    /** Called from android_jni.c (cached as net/vt3port/game/Bridge.openSettings). */
    static void openSettings() {
        Context c = app;
        if (c == null) return;
        try {
            Intent i = new Intent(c, Settings.class);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            c.startActivity(i);
        } catch (RuntimeException e) {
            DiagLog.w("app", "could not open the settings screen: " + e.getMessage());
        }
    }
}
