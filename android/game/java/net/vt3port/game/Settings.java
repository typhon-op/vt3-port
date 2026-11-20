package net.vt3port.game;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.PowerManager;
import android.view.Gravity;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.File;

/**
 * Graphics settings. A preset fills in defaults for every option; each option also has its own
 * override, so a player can take the Quality preset and drop the frame cap to 90, or take Low and
 * raise anisotropy. Nothing is locked by the preset.
 *
 * Resolution and the frame cap are read when the renderer starts, so the screen offers Apply and
 * restart rather than pretending a live change. The game's own graphics options are untouched:
 * this only selects how the port renders the game, not what the game draws.
 */
public class Settings extends Activity {
    /* Values written to the override markers; -1 means "follow the preset / device". */
    static final int[] FPS = { -1, 30, 45, 60, 90, 120, 0 };
    static final String[] FPS_LABEL = { "Auto", "30 fps", "45 fps", "60 fps", "90 fps", "120 fps", "Uncapped" };
    static final int[] RES = { -1, 0, 1440, 1220, 1080, 900, 720, 540 };
    static final String[] RES_LABEL = { "Auto", "Native panel", "1440p", "1220p", "1080p", "900p", "720p", "540p" };
    static final int[] ANISO = { -1, 1, 2, 4, 8, 16 };
    static final String[] ANISO_LABEL = { "Auto", "1x", "2x", "4x", "8x", "16x" };

    File root;
    Spinner preset, fps, res, aniso;
    boolean settingUp;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        root = getExternalFilesDir(null);
        /* Effective values reflect the tier and the current thermal status before anything is shown. */
        GraphicsProfile.apply(this, root);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(getColor(R.color.vt3_page));
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(48, 40, 48, 40);
        scroll.addView(box);

        TextView title = new TextView(this);
        title.setText(R.string.settings_title);
        title.setTextSize(24);
        title.setTextColor(getColor(R.color.vt3_text));
        box.addView(title);

        TextView info = new TextView(this);
        info.setText(describe());
        info.setTextColor(getColor(R.color.vt3_muted));
        info.setPadding(0, 12, 0, 24);
        box.addView(info);

        settingUp = true;
        preset = row(box, R.string.settings_preset, GraphicsProfile.PRESET_LABELS,
                presetIndex(GraphicsProfile.read(root, "preset", "auto")));
        fps = row(box, R.string.settings_fps, FPS_LABEL, indexOf(FPS, GraphicsProfile.override(root, "override_fps")));
        res = row(box, R.string.settings_resolution, RES_LABEL,
                indexOf(RES, GraphicsProfile.override(root, "override_res")));
        aniso = row(box, R.string.settings_aniso, ANISO_LABEL,
                indexOf(ANISO, GraphicsProfile.override(root, "override_aniso")));
        settingUp = false;

        preset.setOnItemSelectedListener(new Pick() {
            void take(int i) {
                GraphicsProfile.write(root, "preset", GraphicsProfile.PRESET_IDS[i]);
                GraphicsProfile.write(root, "override_fps", null);   /* a preset is a fresh set of defaults */
                GraphicsProfile.write(root, "override_res", null);
                GraphicsProfile.write(root, "override_aniso", null);
                GraphicsProfile.write(root, "override_scale", null);
                GraphicsProfile.apply(Settings.this, root);
                reload();
            }
        });
        fps.setOnItemSelectedListener(new Pick() {
            void take(int i) { override("override_fps", FPS[i]); }
        });
        res.setOnItemSelectedListener(new Pick() {
            void take(int i) { override("override_res", RES[i]); }
        });
        aniso.setOnItemSelectedListener(new Pick() {
            void take(int i) { override("override_aniso", ANISO[i]); }
        });

        TextView note = new TextView(this);
        note.setText(R.string.settings_individual);
        note.setTextColor(getColor(R.color.vt3_muted));
        note.setPadding(0, 24, 0, 16);
        box.addView(note);

        Button apply = new Button(this);
        apply.setText(R.string.settings_restart);
        apply.setOnClickListener(v -> {
            GraphicsProfile.apply(this, root);
            ThermalWatch.start(this);
            startActivity(new Intent(this, android.app.NativeActivity.class));
            finish();
        });
        box.addView(apply);

        TextView restartNote = new TextView(this);
        restartNote.setText(R.string.settings_note);
        restartNote.setTextColor(getColor(R.color.vt3_muted));
        box.addView(restartNote);

        setContentView(scroll);
    }

    /* Spinner plumbing: swallow the callback the initial selection fires. */
    abstract class Pick implements AdapterView.OnItemSelectedListener {
        abstract void take(int index);

        @Override
        public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
            if (!settingUp) take(position);
        }

        @Override
        public void onNothingSelected(AdapterView<?> parent) {}
    }

    String describe() {
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        int thermal = pm != null ? pm.getCurrentThermalStatus() : 0;
        String presetNow = GraphicsProfile.read(root, "preset", "auto");
        int tier = GraphicsProfile.presetTier(presetNow);
        if (tier < 0) tier = GraphicsProfile.classify(this);
        return GraphicsProfile.describeDisplay(this) + "\nDevice tier: " + GraphicsProfile.TIER_NAME[tier]
                + "  ·  " + GraphicsProfile.thermalName(thermal);
    }

    Spinner row(LinearLayout box, int label, String[] labels, int selected) {
        TextView head = new TextView(this);
        head.setText(label);
        head.setTextColor(getColor(R.color.vt3_text));
        head.setPadding(0, 16, 0, 4);
        box.addView(head);
        Spinner s = new Spinner(this);
        ArrayAdapter<String> a = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, labels);
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        s.setAdapter(a);
        if (selected >= 0 && selected < labels.length) s.setSelection(selected);
        box.addView(s);
        return s;
    }

    void override(String name, int value) {
        GraphicsProfile.write(root, name, value < 0 ? null : Integer.toString(value));
        GraphicsProfile.apply(this, root);
    }

    /* After a preset change the option rows show the new effective values. */
    void reload() {
        settingUp = true;
        fps.setSelection(indexOf(FPS, GraphicsProfile.override(root, "override_fps")));
        res.setSelection(indexOf(RES, GraphicsProfile.override(root, "override_res")));
        aniso.setSelection(indexOf(ANISO, GraphicsProfile.override(root, "override_aniso")));
        settingUp = false;
    }

    static int presetIndex(String preset) {
        for (int i = 0; i < GraphicsProfile.PRESET_IDS.length; i++)
            if (GraphicsProfile.PRESET_IDS[i].equals(preset)) return i;
        return 0;
    }

    static int indexOf(int[] values, Integer v) {
        if (v == null) return 0;
        for (int i = 0; i < values.length; i++) if (values[i] == v) return i;
        return 0;
    }
}
