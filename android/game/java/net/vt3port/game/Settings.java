package net.vt3port.game;

import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.PowerManager;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.List;

/**
 * The settings screen: horizontal categories, vertically scrolling content, every row backed by a
 * marker the native runtime actually reads.
 *
 * Nothing here is decorative. Each control writes one of the markers listed in GraphicsProfile or
 * read directly in android_entry.c, so a switch that does nothing cannot appear: if the engine has
 * no option for something (multisampling, frame generation, FSR, a frame-pacing mode, crowd
 * rendering, a low-memory mode), the row is absent rather than present and inert.
 */
public class Settings extends BaseActivity {
    static final int TAB_GRAPHICS = 0, TAB_CONTROLS = 1, TAB_ADVANCED = 2, TAB_GAME = 3, TAB_DRIVER = 4;
    static final int PICK_DRIVER = 11;

    File root;
    LinearLayout tabs, content;
    int tab = TAB_GRAPHICS;
    TextView driverStatus;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        root = Game.root(this);
        DiagLog.i("settings", "settings screen opened");
        /* Effective values reflect the tier and the current thermal status before anything is shown. */
        GraphicsProfile.apply(this, root);
        setContentView(build());
        showTab(TAB_GRAPHICS);
    }

    @Override
    protected void onStart() {
        super.onStart();
        Bridge.screenOpened();
    }

    @Override
    protected void onStop() {
        Bridge.screenClosed();
        super.onStop();
    }

    View build() {
        LinearLayout page = Ui.column(this);
        page.setBackgroundColor(getColor(R.color.vt3_page));

        /* Header: the selection orange, white capitals, and the way back. */
        LinearLayout head = Ui.rowOf(this);
        head.setBackgroundResource(R.drawable.bg_tab_selected);
        head.setPadding(Ui.dp(this, 16), Ui.dp(this, 12), Ui.dp(this, 12), Ui.dp(this, 12));
        TextView title = Ui.title(this, getString(R.string.settings_title));
        head.addView(title, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView back = Ui.text(this, getString(R.string.settings_close), 12, 0xFFFFFFFF, true);
        back.setAllCaps(true);
        back.setLetterSpacing(0.1f);
        back.setPadding(Ui.dp(this, 12), Ui.dp(this, 6), Ui.dp(this, 6), Ui.dp(this, 6));
        back.setOnClickListener(v -> finish());
        head.addView(back);
        page.addView(head);

        /* The category strip stays put while the content scrolls under it. */
        HorizontalScrollView strip = new HorizontalScrollView(this);
        strip.setHorizontalScrollBarEnabled(false);
        strip.setBackgroundColor(getColor(R.color.vt3_page_deep));
        tabs = Ui.rowOf(this);
        tabs.setPadding(Ui.dp(this, 12), Ui.dp(this, 8), Ui.dp(this, 12), Ui.dp(this, 8));
        strip.addView(tabs);
        page.addView(strip, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        String[] names = {
                getString(R.string.tab_graphics), getString(R.string.tab_controls),
                getString(R.string.tab_advanced), getString(R.string.tab_game),
                getString(R.string.tab_driver)
        };
        for (int i = 0; i < names.length; i++) {
            final int index = i;
            TextView t = Ui.text(this, names[i], 12, getColor(R.color.vt3_ink), true);
            t.setAllCaps(true);
            t.setLetterSpacing(0.06f);
            t.setBackgroundResource(R.drawable.bg_tab);
            t.setPadding(Ui.dp(this, 14), Ui.dp(this, 9), Ui.dp(this, 14), Ui.dp(this, 9));
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            p.rightMargin = Ui.dp(this, 6);
            t.setOnClickListener(v -> showTab(index));
            tabs.addView(t, p);
        }

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setVerticalScrollBarEnabled(true);
        content = Ui.column(this);
        int pad = Ui.dp(this, 14);
        /* The bottom padding keeps the last row clear of the navigation bar: the decor fits the
         * system bars, and this is the extra room the scroll view needs to show its end. */
        content.setPadding(pad, pad, pad, Ui.dp(this, 28));
        scroll.addView(content, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        page.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        return page;
    }

    void showTab(int index) {
        tab = index;
        for (int i = 0; i < tabs.getChildCount(); i++) {
            TextView t = (TextView) tabs.getChildAt(i);
            boolean on = i == index;
            t.setBackgroundResource(on ? R.drawable.bg_tab_selected : R.drawable.bg_tab);
            t.setTextColor(on ? 0xFFFFFFFF : 0xFF2B2723);
        }
        content.removeAllViews();
        switch (index) {
            case TAB_CONTROLS: controlsTab(); break;
            case TAB_ADVANCED: advancedTab(); break;
            case TAB_GAME: gameFilesTab(); break;
            case TAB_DRIVER: driverTab(); break;
            default: graphicsTab(); break;
        }
        content.scrollTo(0, 0);
    }

    /* ---- Graphics & Performance --------------------------------------------------------- */

    void graphicsTab() {
        String preset = Markers.read(root, "preset", "auto");

        content.addView(Ui.section(this, getString(R.string.settings_quality)));
        Ui.Choice pre = new Ui.Choice(this, GraphicsProfile.PRESET_LABELS,
                presetIndex(preset), () -> {});
        pre.setOnClickListener(v -> {
            pre.step(1);
            applyPreset(pre.index);
        });
        content.addView(Ui.row(this, getString(R.string.settings_preset),
                getString(R.string.settings_preset_hint), pre));

        content.addView(Ui.section(this, getString(R.string.settings_display)));

        Ui.Choice res = new Ui.Choice(this, GraphicsProfile.RES_LABEL,
                indexOf(GraphicsProfile.RES, Markers.readInt(root, "override_res", null)), () -> {});
        res.setOnClickListener(v -> {
            res.step(1);
            setOverride("override_res", GraphicsProfile.RES[res.index]);
        });
        content.addView(Ui.row(this, (getString(R.string.settings_resolution) + " \u00b7 next match"),
                getString(R.string.settings_resolution_hint), res));

        int scale = Markers.intOr(root, "override_scale", 100);
        content.addView(Ui.slider(this, getString(R.string.settings_scale),
                getString(R.string.settings_scale_hint), 60, 100, 5, scale, " %",
                v -> setOverride("override_scale", v)));

        Ui.Choice aniso = new Ui.Choice(this, GraphicsProfile.ANISO_LABEL,
                indexOf(GraphicsProfile.ANISO, Markers.readInt(root, "override_aniso", null)), () -> {});
        aniso.setOnClickListener(v -> {
            aniso.step(1);
            setOverride("override_aniso", GraphicsProfile.ANISO[aniso.index]);
        });
        content.addView(Ui.row(this, (getString(R.string.settings_aniso) + " \u00b7 next match"),
                getString(R.string.settings_aniso_hint), aniso));

        content.addView(toggleRow((getString(R.string.settings_widescreen) + " \u00b7 next match"),
                getString(R.string.settings_widescreen_hint),
                Markers.flag(root, "override_widescreen", true),
                on -> { Markers.writeFlag(root, "override_widescreen", on); custom(); }));

        content.addView(toggleRow((getString(R.string.settings_shadows) + " \u00b7 next match"),
                getString(R.string.settings_shadows_hint),
                Markers.flag(root, "override_shadows", GraphicsProfile.presetTier(preset) <= GraphicsProfile.HIGH),
                on -> { Markers.writeFlag(root, "override_shadows", on); custom(); }));

        content.addView(Ui.section(this, getString(R.string.settings_frames)));
        Ui.Choice fps = new Ui.Choice(this, GraphicsProfile.FPS_LABEL,
                indexOf(GraphicsProfile.FPS, Markers.readInt(root, "override_fps", null)), () -> {});
        fps.setOnClickListener(v -> {
            fps.step(1);
            setOverride("override_fps", GraphicsProfile.FPS[fps.index]);
        });
        content.addView(Ui.row(this, (getString(R.string.settings_fps) + " \u00b7 next match"),
                getString(R.string.settings_fps_hint), fps));
        content.addView(Ui.note(this, getString(R.string.settings_pacing_note)));
        content.addView(Ui.note(this, "Frame limit, resolution, render scale, anisotropy, shadows and "
                + "widescreen are read when a match starts: a change applies to the next match, "
                + "including one started after returning to the main menu."));

        content.addView(Ui.section(this, getString(R.string.settings_overlay)));
        content.addView(toggleRow(getString(R.string.settings_overlay_on),
                getString(R.string.settings_overlay_hint),
                Markers.flag(root, "perf_overlay", false),
                on -> Markers.writeFlag(root, "perf_overlay", on)));
        Ui.Choice overlay = new Ui.Choice(this,
                new String[] { getString(R.string.settings_overlay_compact),
                        getString(R.string.settings_overlay_full) },
                Markers.flag(root, "perf_overlay_full", false) ? 1 : 0, () -> {});
        overlay.setOnClickListener(v -> {
            overlay.step(1);
            Markers.writeFlag(root, "perf_overlay_full", overlay.index == 1);
        });
        content.addView(Ui.row(this, getString(R.string.settings_overlay_size),
                getString(R.string.settings_overlay_size_hint), overlay));

        /* Where the overlay sits. The runtime reads overlay_pos once a second, so it moves without a
         * restart; the corners are the four places the touch controls never occupy. */
        String[] corners = { "Top left", "Top right", "Bottom left", "Bottom right" };
        final int[] corner = { Markers.intOr(root, "overlay_pos", 0) };
        if (corner[0] < 0 || corner[0] > 3) corner[0] = 0;
        Ui.Choice where = new Ui.Choice(this, corners, corner[0], () -> {});
        where.setOnClickListener(v -> {
            where.step(1);
            Markers.writeInt(root, "overlay_pos", where.index);
        });
        content.addView(Ui.row(this, "Overlay position",
                "Keeps the overlay clear of the on-screen controls", where));

        content.addView(Ui.section(this, getString(R.string.settings_thermal)));
        content.addView(toggleRow(getString(R.string.settings_thermal_guard),
                getString(R.string.settings_thermal_guard_hint),
                Markers.flag(root, "thermal_guard", true),
                on -> { Markers.writeFlag(root, "thermal_guard", on); GraphicsProfile.apply(this, root); }));
        int thermal = GraphicsProfile.thermalStatus(this);
        content.addView(Ui.row(this, getString(R.string.settings_thermal_state),
                getString(R.string.settings_device_hint),
                Ui.value(this, (thermal < 0 ? getString(R.string.settings_unknown)
                        : GraphicsProfile.thermalName(thermal)) + "  ·  "
                        + GraphicsProfile.TIER_NAME[deviceTier()])));

        content.addView(Ui.note(this, GraphicsProfile.describeDevice(this) + "\n"
                + GraphicsProfile.describeDisplay(this) + "\n"
                + getString(R.string.settings_effective,
                Markers.read(root, "fps_cap", "auto"),
                Markers.read(root, "render_h", getString(R.string.settings_native)),
                Markers.read(root, "aniso", "auto"),
                Markers.read(root, "render_scale_pct", "100"))));

        content.addView(Ui.primary(this, getString(R.string.settings_restart), v -> {
            DiagLog.i("settings", "applying graphics settings and restarting the game");
            GraphicsProfile.apply(this, root);
            ThermalWatch.start(this);
            Game.launch(this);
        }));
        content.addView(Ui.note(this, getString(R.string.settings_note)));
    }

    int deviceTier() {
        String preset = Markers.read(root, "preset", "auto");
        int tier = GraphicsProfile.presetTier(preset);
        return tier >= 0 ? tier : GraphicsProfile.classify(this);
    }

    void applyPreset(int index) {
        String id = GraphicsProfile.PRESET_IDS[index];
        DiagLog.i("graphics", "preset " + id);
        Markers.write(root, "preset", id);
        /* A preset is a fresh set of defaults: the per-option overrides it replaces are cleared. */
        Markers.write(root, "override_fps", null);
        Markers.write(root, "override_res", null);
        Markers.write(root, "override_aniso", null);
        Markers.write(root, "override_scale", null);
        Markers.write(root, "override_shadows", null);
        Markers.write(root, "override_widescreen", null);
        GraphicsProfile.apply(this, root);
        showTab(TAB_GRAPHICS);
    }

    /** An individual change makes the preset Custom, rather than leaving a name that no longer fits. */
    void custom() {
        if (!"custom".equals(Markers.read(root, "preset", "auto")))
            Markers.write(root, "preset", "custom");
        GraphicsProfile.apply(this, root);
    }

    void setOverride(String name, int value) {
        Markers.write(root, name, value < 0 ? null : Integer.toString(value));
        custom();
    }

    /* ---- Controls & Sound ---------------------------------------------------------------- */

    void controlsTab() {
        content.addView(Ui.section(this, getString(R.string.settings_touch)));
        content.addView(toggleRow(getString(R.string.settings_touch_on),
                getString(R.string.settings_touch_hint),
                Markers.flag(root, "touch", true),
                on -> Markers.writeFlag(root, "touch", on)));
        content.addView(toggleRow(getString(R.string.settings_touch_force),
                getString(R.string.settings_touch_force_hint),
                Markers.flag(root, "touch_force", false),
                on -> Markers.writeFlag(root, "touch_force", on)));

        /* Percent integers, which is what android_entry.c reads for VT3_TOUCH_SCALE/_ALPHA. */
        int size = Markers.intOr(root, "touch_scale_pct", 100);
        content.addView(Ui.slider(this, getString(R.string.settings_touch_size),
                getString(R.string.settings_touch_size_hint), 60, 160, 10, size, " %",
                v -> Markers.writeInt(root, "touch_scale_pct", v)));

        int alpha = Markers.intOr(root, "touch_alpha_pct", 100);
        content.addView(Ui.slider(this, getString(R.string.settings_touch_alpha),
                getString(R.string.settings_touch_alpha_hint), 30, 100, 10, alpha, " %",
                v -> Markers.writeInt(root, "touch_alpha_pct", v)));

        content.addView(toggleRow(getString(R.string.settings_touch_mono),
                getString(R.string.settings_touch_mono_hint),
                Markers.flag(root, "touch_mono", false),
                on -> Markers.writeFlag(root, "touch_mono", on)));

        content.addView(Ui.button(this, "Customize controls, profiles and layouts",
                v -> startActivity(new Intent(this, TouchControls.class))));
        content.addView(Ui.button(this, getString(R.string.settings_controls_guide),
                v -> controlsGuide()));
        content.addView(Ui.note(this, "Control size and opacity are read when a match starts and apply "
                + "to the next one; the layout, profiles and the swipe pad are applied live."));
        content.addView(Ui.note(this, getString(R.string.settings_controls_note)));

        content.addView(Ui.section(this, getString(R.string.settings_sound)));
        int vol = Markers.intOr(root, "audio_volume", 100);
        content.addView(Ui.slider(this, getString(R.string.settings_volume),
                getString(R.string.settings_volume_hint), 0, 100, 5, vol, " %",
                v -> Markers.writeInt(root, "audio_volume", v)));
        content.addView(toggleRow(getString(R.string.settings_mute),
                getString(R.string.settings_mute_hint),
                Markers.flag(root, "audio_mute", false),
                on -> Markers.writeFlag(root, "audio_mute", on)));
        content.addView(Ui.note(this, getString(R.string.settings_audio_note)));
    }

    /** The real mapping the DirectInput shim uses, not a decorative diagram. */
    void controlsGuide() {
        String[] lines = getResources().getStringArray(R.array.controls_guide);
        StringBuilder s = new StringBuilder();
        for (String line : lines) s.append("\u2022 ").append(line).append('\n');
        new AlertDialog.Builder(this)
                .setTitle(R.string.settings_controls_guide)
                .setMessage(s.toString().trim())
                .setPositiveButton(android.R.string.ok, null)
                .show();
    }

    /* ---- Advanced ------------------------------------------------------------------------ */

    void advancedTab() {
        boolean detail = Markers.flag(root, "diag_detail", false);
        content.addView(Ui.section(this, getString(R.string.settings_rendering)));
        content.addView(Ui.row(this, getString(R.string.settings_renderer),
                getString(R.string.settings_renderer_hint),
                Ui.value(this, "Vulkan")));
        String vk = GraphicsProfile.vulkanInfo(this);
        content.addView(Ui.row(this, getString(R.string.settings_vulkan),
                getString(R.string.settings_vulkan_hint),
                Ui.value(this, vk != null ? vk : getString(R.string.settings_unknown))));
        content.addView(Ui.row(this, getString(R.string.settings_shader_pack),
                getString(R.string.settings_shader_pack_hint),
                Ui.value(this, new File(root, "shaders.pack").isFile()
                        ? Adreno.bytes(new File(root, "shaders.pack").length())
                        : getString(R.string.settings_missing))));

        content.addView(Ui.section(this, getString(R.string.settings_memory)));
        content.addView(Ui.row(this, getString(R.string.settings_ram),
                getString(R.string.settings_ram_hint),
                Ui.value(this, freeRam())));
        content.addView(Ui.row(this, getString(R.string.settings_free),
                getString(R.string.settings_free_hint),
                Ui.value(this, Adreno.bytes(StorageAccess.freeBytes(Game.dataRoot(this))))));

        content.addView(Ui.section(this, getString(R.string.settings_developer)));
        content.addView(toggleRow(getString(R.string.diagnostics_detail),
                getString(R.string.settings_detail_hint), detail,
                on -> Markers.writeFlag(root, "diag_detail", on)));
        if (detail) {
            content.addView(toggleRow(getString(R.string.settings_trace_audio),
                    getString(R.string.settings_trace_audio_hint),
                    Markers.flag(root, "trace_audio", false),
                    on -> Markers.writeFlag(root, "trace_audio", on)));
            content.addView(toggleRow(getString(R.string.settings_trace_fps),
                    getString(R.string.settings_trace_fps_hint),
                    Markers.flag(root, "trace_fps", false),
                    on -> Markers.writeFlag(root, "trace_fps", on)));
            content.addView(Ui.note(this, getString(R.string.settings_developer_note)));
        } else {
            content.addView(Ui.note(this, getString(R.string.settings_developer_hidden)));
        }

        content.addView(Ui.section(this, getString(R.string.settings_maintenance)));
        content.addView(Ui.button(this, getString(R.string.settings_diagnostics),
                v -> startActivity(new Intent(this, Diagnostics.class))));
        content.addView(Ui.button(this, getString(R.string.settings_reset_graphics), v -> {
            Markers.write(root, "preset", "auto");
            for (String k : new String[] { "override_fps", "override_res", "override_aniso",
                    "override_scale", "override_shadows", "override_widescreen" })
                Markers.write(root, k, null);
            GraphicsProfile.apply(this, root);
            showTab(TAB_GRAPHICS);
        }));
        content.addView(Ui.button(this, getString(R.string.settings_reinstall),
                v -> changeInstallation()));
    }

    String freeRam() {
        android.app.ActivityManager am =
                (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
        if (am == null) return getString(R.string.settings_unknown);
        android.app.ActivityManager.MemoryInfo m = new android.app.ActivityManager.MemoryInfo();
        am.getMemoryInfo(m);
        return Adreno.bytes(m.availMem) + " / " + Adreno.bytes(m.totalMem);
    }

    void changeInstallation() {
        new AlertDialog.Builder(this)
                .setMessage(R.string.settings_reinstall_ask)
                .setPositiveButton(R.string.settings_reinstall, (d, w) -> {
                    DiagLog.i("install", "change installation requested; clearing install.ok and game_root");
                    Game.forget(this);
                    startActivity(new Intent(this, Installer.class));
                    finish();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    /* ---- Game Files ---------------------------------------------------------------------- */

    void gameFilesTab() {
        String problem = Game.problem(this);
        File data = Game.dataRoot(this);

        content.addView(Ui.section(this, getString(R.string.settings_game_source)));
        content.addView(Ui.row(this, getString(R.string.settings_game_folder),
                data.getAbsolutePath(),
                Ui.value(this, problem == null ? getString(R.string.settings_ok)
                        : getString(R.string.settings_problem))));
        content.addView(Ui.row(this, getString(R.string.settings_validation),
                problem == null ? getString(R.string.settings_validation_ok) : problem,
                Ui.value(this, GameFolder.isDataRoot(data) ? "Resource/" : "\u2014")));
        content.addView(Ui.row(this, getString(R.string.settings_image),
                new File(root, "image.bin").getAbsolutePath(),
                Ui.value(this, new File(root, "image.bin").isFile()
                        ? Adreno.bytes(new File(root, "image.bin").length())
                        : getString(R.string.settings_missing))));
        content.addView(Ui.row(this, getString(R.string.settings_install_folder),
                getString(R.string.settings_install_folder_hint),
                Ui.value(this, StorageAccess.destPath(this) == null
                        ? getString(R.string.dest_default)
                        : StorageAccess.destPath(this).getAbsolutePath())));
        content.addView(Ui.row(this, getString(R.string.settings_access),
                getString(R.string.settings_access_hint),
                Ui.value(this, StorageAccess.allFiles(this)
                        ? getString(R.string.storage_granted) : getString(R.string.storage_denied))));

        content.addView(Ui.button(this, getString(R.string.settings_change_source), v -> {
            /* The install record goes, the files stay: setup can then read another folder or the
             * disc image without deleting anything that is still valid. */
            Game.forget(this);
            startActivity(new Intent(this, Installer.class));
            finish();
        }));
        content.addView(Ui.button(this, getString(R.string.settings_verify), v -> {
            String p = Game.problem(this);
            new AlertDialog.Builder(this)
                    .setTitle(R.string.settings_verify)
                    .setMessage(p == null
                            ? getString(R.string.settings_verify_ok, data.getAbsolutePath())
                            : p)
                    .setPositiveButton(android.R.string.ok, null)
                    .show();
        }));
        content.addView(Ui.button(this, getString(R.string.storage_allow),
                v -> startActivity(StorageAccess.settings(this))));
        content.addView(Ui.note(this, getString(R.string.settings_game_note)));
    }

    /* ---- Adreno Tools -------------------------------------------------------------------- */

    void driverTab() {
        content.addView(Ui.section(this, getString(R.string.tab_driver)));
        String cur = Adreno.current(this);
        driverStatus = Ui.text(this, cur == null ? getString(R.string.driver_system)
                : getString(R.string.driver_imported, cur), 13, getColor(R.color.vt3_ink), true);
        content.addView(driverStatus);
        if (cur != null) {
            String info = Adreno.describe(this, cur);
            content.addView(Ui.note(this, info == null ? cur : info));
        }
        content.addView(Ui.note(this, Adreno.limitation()));

        content.addView(Ui.button(this, getString(R.string.driver_import), v -> {
            try {
                startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                        .addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), PICK_DRIVER);
            } catch (Throwable e) {
                toast(getString(R.string.driver_no_picker));
            }
        }));
        content.addView(Ui.button(this, getString(R.string.driver_use_system), v -> {
            Adreno.useSystem(this);
            showTab(TAB_DRIVER);
        }));
        content.addView(Ui.button(this, getString(R.string.driver_rollback), v -> {
            boolean ok = Adreno.rollback(this);
            toast(getString(ok ? R.string.driver_rolled_back : R.string.driver_no_backup));
            showTab(TAB_DRIVER);
        }));
        content.addView(Ui.button(this, getString(R.string.driver_clear), v -> {
            Adreno.clearAll(this);
            toast(getString(R.string.driver_cleared));
            showTab(TAB_DRIVER);
        }));

        List<String> all = Adreno.list(this);
        if (!all.isEmpty()) {
            content.addView(Ui.section(this, getString(R.string.driver_stored)));
            StringBuilder s = new StringBuilder();
            for (String n : all) s.append("\u2022 ").append(n).append('\n');
            content.addView(Ui.note(this, s.toString().trim()));
        }
        content.addView(Ui.note(this, getString(R.string.driver_note)));
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        if (code != PICK_DRIVER || result != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;
        Adreno.Result r = Adreno.inspect(this, uri);
        StringBuilder s = new StringBuilder();
        s.append(getString(R.string.driver_name, r.name == null ? "?" : r.name)).append('\n');
        s.append(getString(R.string.driver_library, r.file == null ? "\u2014" : r.file)).append('\n');
        s.append(getString(R.string.driver_abi, r.abi == null ? "\u2014" : r.abi)).append('\n');
        s.append(getString(R.string.driver_version, r.version)).append('\n');
        s.append(getString(R.string.driver_size, Adreno.bytes(r.bytes))).append('\n');
        s.append(getString(R.string.driver_sha, r.sha256 == null ? "\u2014" : r.sha256)).append("\n\n");
        s.append(Adreno.limitation());
        AlertDialog.Builder b = new AlertDialog.Builder(this)
                .setTitle(r.ok() ? R.string.driver_valid : R.string.driver_invalid)
                .setMessage(s.toString())
                .setNegativeButton(android.R.string.cancel, null);
        if (r.ok()) {
            b.setPositiveButton(R.string.driver_keep, (d, w) -> {
                try {
                    Adreno.save(this, uri, r);
                    toast(getString(R.string.driver_saved, r.name));
                } catch (Exception e) {
                    toast(getString(R.string.driver_save_failed, String.valueOf(e.getMessage())));
                }
                showTab(TAB_DRIVER);
            });
        }
        b.show();
    }

    /* ---- shared rows --------------------------------------------------------------------- */

    interface Flag {
        void set(boolean on);
    }

    View toggleRow(String label, String hint, boolean on, Flag sink) {
        return Ui.row(this, label, hint, Ui.toggle(this, on, (b, checked) -> {
            sink.set(checked);
            DiagLog.i("settings", label + " = " + checked);
        }));
    }

    void toast(String s) {
        android.widget.Toast.makeText(this, s, android.widget.Toast.LENGTH_LONG).show();
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

    /** Back leaves the screen; the settings are already saved, so nothing is asked. */
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (e.getKeyCode() == KeyEvent.KEYCODE_BUTTON_B
                && e.getAction() == KeyEvent.ACTION_DOWN) {
            finish();
            return true;
        }
        return super.dispatchKeyEvent(e);
    }
}
