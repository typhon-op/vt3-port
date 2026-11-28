package net.vt3port.game;

import android.app.Service;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;

/**
 * Watches the platform thermal status while the game runs and publishes the numbers the in-game
 * overlay shows.
 *
 * Three separate sources, never mixed:
 *   thermal       Android's thermal service (PowerManager), the status the guard steps on.
 *   battery_temp  the battery broadcast's EXTRA_TEMPERATURE, in tenths of a degree.
 *   cpu_temp      a readable /sys/class/thermal zone whose type names a CPU/SoC sensor. Many
 *                 devices restrict sysfs from apps; when no zone is readable the marker is removed
 *                 and the overlay says "Unavailable" instead of showing a guessed or borrowed value.
 *
 * Sampling is every 5 s, on the main looper but with three small file reads, so it cannot disturb
 * rendering or audio. The runtime reads the markers once a second at most.
 */
public class ThermalWatch extends Service implements PowerManager.OnThermalStatusChangedListener {
    static final long PERIOD_MS = 5000;
    /** Zone types that are a CPU/SoC sensor rather than a charger, GPU or skin sensor. */
    static final String[] CPU_HINTS = {
            "cpu", "soc", "ap", "tsens", "mtktscpu", "cluster", "big", "little", "silver", "gold"
    };

    private PowerManager pm;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable sample = new Runnable() {
        @Override
        public void run() {
            try {
                sampleTemperatures();
            } catch (Throwable ignored) {
                /* a sensor that cannot be read must never take the game down */
            }
            handler.postDelayed(this, PERIOD_MS);
        }
    };

    public static void start(android.content.Context c) {
        try {
            c.startService(new Intent(c, ThermalWatch.class));
        } catch (Exception ignored) {
            /* the marker simply keeps its previous value; the game still runs */
        }
    }

    /**
     * Stops the watch. Only a running match reads these markers, and a started service left behind
     * keeps the process alive (and restarted) after its task is gone: the app looks closed while it
     * is still running. Called when a match ends and when the app is quit.
     */
    public static void stop(android.content.Context c) {
        try {
            c.stopService(new Intent(c, ThermalWatch.class));
        } catch (Exception ignored) {
            /* nothing to stop, or the service is not running */
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        pm = (PowerManager) getSystemService(POWER_SERVICE);
        if (pm != null) {
            pm.addThermalStatusListener(this);
            write("thermal", Integer.toString(pm.getCurrentThermalStatus()));
        }
        handler.post(sample);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        /* Not sticky: the watch is only useful while a match runs, and a sticky service is restarted
         * by the platform after the process goes, which resurrects the app behind a finished task. */
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        handler.removeCallbacks(sample);
        if (pm != null) pm.removeThermalStatusListener(this);
        super.onDestroy();
    }

    @Override
    public void onThermalStatusChanged(int status) {
        DiagLog.i("thermal", "status " + status + " (" + GraphicsProfile.thermalName(status) + ")");
        write("thermal", Integer.toString(status));
    }

    /** The battery and CPU temperatures, each from its own source, each removed when unreadable. */
    void sampleTemperatures() {
        battery();
        cpu();
        if (pm != null) write("thermal", Integer.toString(pm.getCurrentThermalStatus()));
    }

    private void battery() {
        try {
            Intent b = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (b == null) {
                remove("battery_temp");
                return;
            }
            int tenths = b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
            if (tenths == Integer.MIN_VALUE || tenths == 0) {
                remove("battery_temp");
                return;
            }
            write("battery_temp", String.format(Locale.US, "%.1f", tenths / 10.0));
        } catch (RuntimeException e) {
            remove("battery_temp");
        }
    }

    /**
     * The hottest readable CPU/SoC thermal zone. The zone's own type is recorded beside the value,
     * so the overlay can show where the number came from.
     */
    private void cpu() {
        double best = Double.NaN;
        String source = null;
        for (int i = 0; i < 40; i++) {
            String type = read(new File("/sys/class/thermal/thermal_zone" + i + "/type")).trim();
            if (type.isEmpty()) continue;
            String lower = type.toLowerCase(Locale.US);
            boolean cpu = false;
            for (String hint : CPU_HINTS) {
                if (lower.contains(hint)) {
                    cpu = true;
                    break;
                }
            }
            if (!cpu) continue;
            String raw = read(new File("/sys/class/thermal/thermal_zone" + i + "/temp")).trim();
            if (raw.isEmpty()) continue;
            double v;
            try {
                v = Double.parseDouble(raw);
            } catch (NumberFormatException e) {
                continue;
            }
            /* Most kernels report millidegrees; a few report tenths. Anything implausible is
             * ignored rather than shown. */
            if (v > 1000) v /= 1000.0;
            else if (v > 200) v /= 10.0;
            if (v < -40 || v > 150) continue;
            if (Double.isNaN(best) || v > best) {
                best = v;
                source = type;
            }
        }
        if (Double.isNaN(best)) {
            remove("cpu_temp");
            remove("cpu_temp_source");
            return;
        }
        write("cpu_temp", String.format(Locale.US, "%.1f", best));
        if (source != null) write("cpu_temp_source", source);
    }

    private static String read(File f) {
        try (InputStream in = new FileInputStream(f)) {
            byte[] b = new byte[64];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, "UTF-8") : "";
        } catch (IOException | RuntimeException e) {
            return "";
        }
    }

    private void write(String name, String value) {
        File f = new File(getExternalFilesDir(null), name);
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(value.getBytes("UTF-8"));
        } catch (IOException ignored) {
            /* the previous value stays, which only delays a quality step or an overlay reading */
        }
    }

    private void remove(String name) {
        new File(getExternalFilesDir(null), name).delete();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
