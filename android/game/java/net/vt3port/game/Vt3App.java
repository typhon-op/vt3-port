package net.vt3port.game;

import android.app.Application;

/**
 * Starts the diagnostic log before any activity. The system can relaunch the NativeActivity on its
 * own after a process death, and in that case the setup screen never runs: without this the native
 * engine would have nowhere to write until something else initialised the log.
 *
 * It also hands the application context to {@link Bridge}, which is how the native quick menu opens
 * the settings screen.
 */
public class Vt3App extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        DiagLog.init(this);
        Bridge.attach(this);
    }
}
