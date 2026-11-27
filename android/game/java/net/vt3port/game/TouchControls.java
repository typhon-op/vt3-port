package net.vt3port.game;

import android.app.AlertDialog;
import android.content.Intent;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

import java.util.ArrayDeque;

/**
 * Customize Controls: the editor for the on-screen controls.
 *
 * Every change writes touch.cfg through TouchConfig, which the runtime reads on the next frame it
 * draws (vt3_touch_reload), so a layout change reaches the game without restarting it.
 *
 * The layout is locked by default: while locked the editor refuses position, size and removal
 * changes, and the runtime never lets a touch move a control anyway. Unlocking is deliberate and
 * saved. Leaving this screen saves and re-locks, which is what "Lock Layout" promises.
 *
 * Undo and redo are whole-file snapshots: the file is a few hundred bytes, so a stack of them is
 * cheaper and far more reliable than a per-operation inverse.
 */
public class TouchControls extends BaseActivity {
    TouchConfig cfg;
    LinearLayout content;
    TextView status;
    Spinner layoutPick, profilePick, controlPick;
    final ArrayDeque<String> undo = new ArrayDeque<>();
    final ArrayDeque<String> redo = new ArrayDeque<>();
    int editing = 0;          /* index into TouchConfig.CTL_IDS */

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        cfg = TouchConfig.load(this);
        DiagLog.i("touch", "customize controls opened (" + cfg.layout() + ", " + cfg.profile() + ")");
        setContentView(build());
        rebuild();
    }

    View build() {
        LinearLayout page = Ui.column(this);
        page.setBackgroundColor(getColor(R.color.vt3_page));

        LinearLayout head = Ui.rowOf(this);
        head.setBackgroundResource(R.drawable.bg_tab_selected);
        head.setPadding(Ui.dp(this, 16), Ui.dp(this, 12), Ui.dp(this, 12), Ui.dp(this, 12));
        TextView title = Ui.title(this, "TOUCHSCREEN CONTROLS");
        head.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView back = Ui.text(this, "DONE", 12, 0xFFFFFFFF, true);
        back.setPadding(Ui.dp(this, 12), Ui.dp(this, 6), Ui.dp(this, 6), Ui.dp(this, 6));
        back.setOnClickListener(v -> finish());
        head.addView(back);
        page.addView(head);

        status = Ui.note(this, "");
        status.setPadding(Ui.dp(this, 14), Ui.dp(this, 8), Ui.dp(this, 14), Ui.dp(this, 0));
        page.addView(status);

        ScrollView scroll = new ScrollView(this);
        content = Ui.column(this);
        content.setPadding(Ui.dp(this, 14), Ui.dp(this, 8), Ui.dp(this, 14), Ui.dp(this, 28));
        scroll.addView(content, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        page.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        return page;
    }

    /* ---- sections -------------------------------------------------------------------- */

    void rebuild() {
        content.removeAllViews();
        status.setText(cfg.problem != null ? cfg.problem
                : "Layout " + TouchConfig.LAYOUT_NAMES[indexOf(TouchConfig.LAYOUT_IDS, cfg.layout())]
                  + "  •  Stick " + TouchConfig.PROFILE_NAMES[indexOf(TouchConfig.PROFILE_IDS, cfg.profile())]
                  + "  •  " + (cfg.locked() ? "Layout locked" : "Layout UNLOCKED"));

        content.addView(Ui.section(this, "Layout and stick"));

        layoutPick = picker(TouchConfig.LAYOUT_NAMES, indexOf(TouchConfig.LAYOUT_IDS, cfg.layout()), i -> {
            snapshot();
            cfg.set("layout", TouchConfig.LAYOUT_IDS[i]);
            save("layout " + TouchConfig.LAYOUT_IDS[i]);
        });
        content.addView(Ui.row(this, "Layout", "Standard is the original button layout", layoutPick));

        profilePick = picker(TouchConfig.PROFILE_NAMES, indexOf(TouchConfig.PROFILE_IDS, cfg.profile()), i -> {
            snapshot();
            cfg.set("profile", TouchConfig.PROFILE_IDS[i]);
            save("profile " + TouchConfig.PROFILE_IDS[i]);
        });
        content.addView(Ui.row(this, "Stick profile", "Independent of the button layout", profilePick));

        Switch lock = Ui.toggle(this, cfg.locked(), (b, on) -> {
            snapshot();
            cfg.setI("lock", on ? 1 : 0);
            save(on ? "layout locked" : "layout unlocked");
        });
        content.addView(Ui.row(this, "Lock layout", "Locked: no accidental moves while playing", lock));

        content.addView(Ui.section(this, "Stick behaviour"));
        profileSlider("Dead zone", "prof.deadzone", 0, 60, Math.round(cfg.f("p." + cfg.profile() + ".deadzone", 0.14f) * 100), "%",
                v -> cfg.setF("p." + cfg.profile() + ".deadzone", v / 100f));
        profileSlider("Sensitivity", "prof.sens", 50, 200, Math.round(cfg.f("p." + cfg.profile() + ".sensitivity", 1f) * 100), "%",
                v -> cfg.setF("p." + cfg.profile() + ".sensitivity", v / 100f));
        profileSlider("Maximum range", "prof.range", 50, 150, Math.round(cfg.f("p." + cfg.profile() + ".range", 1f) * 100), "%",
                v -> cfg.setF("p." + cfg.profile() + ".range", v / 100f));
        profileToggle("Progressive response", "p." + cfg.profile() + ".progressive");
        profileToggle("Floating stick", "p." + cfg.profile() + ".floating");
        profileToggle("Invert X", "p." + cfg.profile() + ".invertx");
        profileToggle("Invert Y", "p." + cfg.profile() + ".inverty");

        content.addView(Ui.section(this, "Controls in this layout"));
        controlPick = picker(TouchConfig.CTL_NAMES, editing, i -> {
            editing = i;
            rebuild();
        });
        content.addView(Ui.row(this, "Edit control", "Position, size, opacity, visibility", controlPick));

        String ctl = TouchConfig.CTL_IDS[editing];
        boolean locked = cfg.locked();
        content.addView(Ui.note(this, cfg.ctlRemoved(ctl) ? "This control is removed from the layout."
                : cfg.ctlVisible(ctl) ? "Visible in the layout." : "Hidden (kept in the layout, not drawn)."));

        ctlSlider("Position X", 0, 100, Math.round(percentX(ctl) * 100), v -> {
            cfg.detachFromPreset();
            cfg.setF(cfg.key(ctl, "x"), v / 100f);
        }, locked);
        ctlSlider("Position Y", 0, 100, Math.round(percentY(ctl) * 100), v -> {
            cfg.detachFromPreset();
            cfg.setF(cfg.key(ctl, "y"), v / 100f);
        }, locked);
        ctlSlider("Size", 50, 250, Math.round(cfg.ctlSize(ctl) * 100), v -> {
            cfg.detachFromPreset();
            cfg.setF(cfg.key(ctl, "size"), v / 100f);
        }, locked);
        ctlSlider("Opacity", 15, 100, Math.round(cfg.ctlOpacity(ctl) * 100), v -> {
            cfg.detachFromPreset();
            cfg.setF(cfg.key(ctl, "opacity"), v / 100f);
        }, false);

        LinearLayout row = Ui.rowOf(this);
        row.addView(button("Hide", v -> {
            snapshot();
            cfg.setI(cfg.key(ctl, "visible"), 0);
            save("hide " + ctl);
        }));
        row.addView(button("Show", v -> {
            snapshot();
            cfg.setI(cfg.key(ctl, "visible"), 1);
            cfg.setI(cfg.key(ctl, "removed"), 0);
            save("show " + ctl);
        }));
        row.addView(button("Remove", v -> confirm("Remove " + TouchConfig.CTL_NAMES[editing] + " from this layout?",
                () -> {
                    snapshot();
                    cfg.setI(cfg.key(ctl, "removed"), 1);
                    save("remove " + ctl);
                }, locked)));
        row.addView(button("Restore", v -> {
            snapshot();
            cfg.setI(cfg.key(ctl, "removed"), 0);
            cfg.setI(cfg.key(ctl, "visible"), 1);
            save("restore " + ctl);
        }));
        content.addView(row);

        content.addView(Ui.section(this, "One-Button Swipe"));
        Switch swipe = Ui.toggle(this, cfg.swipeEnabled(), (b, on) -> {
            snapshot();
            cfg.setI("swipe.enabled", on ? 1 : 0);
            if (on) cfg.setI(cfg.key("swipe", "removed"), 0);
            save("swipe " + (on ? "on" : "off"));
        });
        content.addView(Ui.row(this, "Enable swipe pad", "Swipes send the mapped shot button", swipe));
        profileSlider("Swipe distance", "swipe.dist", 2, 50,
                Math.round(cfg.f("swipe.min_dist", 0.18f) * 100), "%",
                v -> cfg.setF("swipe.min_dist", v / 100f));
        content.addView(Ui.note(this, "Swipe up/down/left/right send "
                + cfg.s("swipe.map.up", "a").toUpperCase() + "/" + cfg.s("swipe.map.down", "b").toUpperCase()
                + "/" + cfg.s("swipe.map.left", "x").toUpperCase() + "/" + cfg.s("swipe.map.right", "y").toUpperCase()
                + ". Holding the finger keeps that button held, which is how the gamepad's own hold behaves."));

        content.addView(Ui.section(this, "Preview and reset"));
        content.addView(button("Preview and test controls", v -> {
            Intent i = new Intent(this, TouchPreview.class);
            startActivity(i);
        }));
        LinearLayout row2 = Ui.rowOf(this);
        row2.addView(button("Undo", v -> {
            if (undo.isEmpty()) return;
            redo.push(cfg.snapshot());
            cfg.restoreSnapshot(undo.pop());
            save("undo");
        }));
        row2.addView(button("Redo", v -> {
            if (redo.isEmpty()) return;
            undo.push(cfg.snapshot());
            cfg.restoreSnapshot(redo.pop());
            save("redo");
        }));
        content.addView(row2);

        LinearLayout row3 = Ui.rowOf(this);
        row3.addView(button("Reset layout", v -> confirm("Reset this layout to its defaults?", () -> {
            snapshot();
            cfg.resetLayout();
            save("reset layout");
        }, false)));
        row3.addView(button("Reset profile", v -> {
            snapshot();
            cfg.resetProfile();
            save("reset profile");
        }));
        content.addView(row3);

        LinearLayout row4 = Ui.rowOf(this);
        row4.addView(button("Backup", v -> {
            boolean ok = cfg.save(this);
            toast(ok ? "Configuration saved as the backup" : "Could not save the configuration");
        }));
        row4.addView(button("Restore backup", v -> confirm("Replace the current layout with the last saved backup?",
                () -> {
                    if (cfg.restoreBackup(this)) {
                        cfg = TouchConfig.load(this);
                        undo.clear();
                        redo.clear();
                        rebuild();
                    } else {
                        toast("There is no usable backup");
                    }
                }, false)));
        content.addView(row4);

        content.addView(Ui.note(this, "Settings are saved automatically. The game reads touch.cfg while it "
                + "runs, so changes apply to the next frame; the stick's analog/d-pad mode follows the game "
                + "on its own."));
    }

    /* ---- helpers --------------------------------------------------------------------- */

    interface IntConsumer { void accept(int v); }
    interface Runnable0 { void run(); }

    float percentX(String ctl) {
        float x = cfg.ctlX(ctl);
        return x < 0 ? 0.5f : x;      /* -1 means "the built-in position" */
    }

    float percentY(String ctl) {
        float y = cfg.ctlY(ctl);
        return y < 0 ? 0.5f : y;
    }

    Spinner picker(String[] labels, int selected, IntConsumer onPick) {
        Spinner s = Ui.spinner(this, labels, Math.max(0, selected));
        s.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> p, View v, int pos, long id) {
                if (pos != selected) onPick.accept(pos);
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> p) {}
        });
        return s;
    }

    Button button(String label, View.OnClickListener l) {
        Button b = Ui.button(this, label, l);
        b.setAllCaps(false);
        return b;
    }

    void profileSlider(String label, String tag, int min, int max, int value, String suffix, IntConsumer onSet) {
        content.addView(slider(label, min, max, value, suffix, v -> {
            snapshotOnce(tag);
            onSet.accept(v);
            cfg.save(this);
        }));
    }

    void ctlSlider(String label, int min, int max, int value, IntConsumer onSet, boolean locked) {
        if (locked) {
            content.addView(Ui.note(this, label + ": unlock the layout to change it."));
            return;
        }
        content.addView(slider(label, min, max, value, "%", v -> {
            onSet.accept(v);
            cfg.save(this);
        }));
    }

    void profileToggle(String label, String key) {
        Switch sw = Ui.toggle(this, cfg.i(key, 0) != 0, (b, on) -> {
            snapshot();
            cfg.setI(key, on ? 1 : 0);
            save(label + (on ? " on" : " off"));
        });
        content.addView(Ui.row(this, label, "", sw));
    }

    /* One snapshot per drag, not per pixel of slider movement. */
    String openTag;
    void snapshotOnce(String tag) {
        if (tag.equals(openTag)) return;
        openTag = tag;
        snapshot();
    }

    LinearLayout slider(String label, int min, int max, int value, String suffix, IntConsumer onSet) {
        LinearLayout box = Ui.column(this);
        TextView t = Ui.text(this, label + "   " + value + suffix, 13, getColor(R.color.vt3_ink), false);
        SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(Math.max(0, Math.min(max - min, value - min)));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean fromUser) {
                t.setText(label + "   " + (p + min) + suffix);
                if (fromUser) onSet.accept(p + min);
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) { openTag = null; }
        });
        box.addView(t);
        box.addView(bar);
        return box;
    }

    void snapshot() {
        undo.push(cfg.snapshot());
        if (undo.size() > 40) undo.removeLast();
        redo.clear();
    }

    void save(String what) {
        boolean ok = cfg.save(this);
        DiagLog.i("touch", what + (ok ? "" : " (save failed)"));
        rebuild();
        if (!ok) toast("Could not save: the previous configuration is still in place");
    }

    void confirm(String question, Runnable0 yes, boolean locked) {
        if (locked) {
            toast("Layout is locked: unlock it first");
            return;
        }
        new AlertDialog.Builder(this)
                .setMessage(question)
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(android.R.string.ok, (d, w) -> yes.run())
                .show();
    }

    void toast(String s) {
        android.widget.Toast.makeText(this, s, android.widget.Toast.LENGTH_SHORT).show();
    }

    static int indexOf(String[] a, String v) {
        for (int i = 0; i < a.length; i++) if (a[i].equals(v)) return i;
        return 0;
    }

    @Override
    protected void onPause() {
        super.onPause();
        // Leaving the editor saves and re-locks, so the layout cannot be nudged in a match.
        cfg.setI("lock", 1);
        cfg.save(this);
        DiagLog.i("touch", "customize controls closed; layout locked");
    }
}
