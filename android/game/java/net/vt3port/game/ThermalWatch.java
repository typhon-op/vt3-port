package net.vt3port.game;

import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.IBinder;
import android.os.PowerManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

/**
 * Watches the platform thermal status while the game runs and writes it to the "thermal" marker.
 *
 * The runtime polls that marker once a second and lowers anisotropic filtering as the phone heats,
 * and the profile uses it on the next launch to move the resolution one rung. Both are gradual by
 * design: the status changes in steps, and the resolution only ever moves at start-up.
 */
public class ThermalWatch extends Service implements PowerManager.OnThermalStatusChangedListener {
    private PowerManager pm;

    public static void start(Context c) {
        try {
            c.startService(new Intent(c, ThermalWatch.class));
        } catch (Exception ignored) {
            /* the marker simply keeps its previous value; the game still runs */
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        pm = (PowerManager) getSystemService(POWER_SERVICE);
        if (pm == null) return;
        pm.addThermalStatusListener(this);
        write(pm.getCurrentThermalStatus());
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        if (pm != null) pm.removeThermalStatusListener(this);
        super.onDestroy();
    }

    @Override
    public void onThermalStatusChanged(int status) {
        write(status);
    }

    private void write(int status) {
        DiagLog.i("thermal", "status " + status + " (" + GraphicsProfile.thermalName(status) + ")");
        File f = new File(getExternalFilesDir(null), "thermal");
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(Integer.toString(status).getBytes("UTF-8"));
        } catch (IOException ignored) {
            /* nothing to do: the previous value stays, which only delays a quality step */
        }
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
