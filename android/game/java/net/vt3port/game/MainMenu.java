package net.vt3port.game;

import android.content.Intent;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * The menu after setup: Start Game, Settings, Language, Quit.
 *
 * The install is re-checked every time the screen comes back, so a folder that was moved or a card
 * that was removed sends the player to setup instead of into a game that cannot start. The chosen
 * game folder is not re-picked: it lives in the install markers (see {@link Game}).
 *
 * Look: the game's own main menu. The artwork sits behind a scrim, the brand is at the top left,
 * the shoulder caps at the top corners, and the actions are red slanted bars stacked in the middle.
 * Quit is the last bar, under Language: the exit is never where the player's thumb lands first.
 */
public class MainMenu extends BaseActivity {
    final List<View> bars = new ArrayList<>();
    int selected;
    TextView footer;
    View startBar;

    /** The one action that ends the app on purpose: leaving a match never does this. */
    void quit() {
        DiagLog.i("app", "quit from the main menu");
        finishAffinity();
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        /* Missing or unreadable game files: back to setup, never into a failed launch. */
        String problem = Game.problem(this);
        if (problem != null) {
            DiagLog.i("menu", "no playable install (" + problem + "); opening setup");
            startActivity(new Intent(this, Installer.class));
            finish();
            return;
        }
        setContentView(build());
        select(0);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (footer == null) return;
        String problem = Game.problem(this);
        if (problem != null) {
            startActivity(new Intent(this, Installer.class));
            finish();
            return;
        }
        footer.setText(getString(R.string.menu_footer, Game.dataRoot(this).getAbsolutePath()));
    }

    View build() {
        FrameLayout root = new FrameLayout(this);

        /* The artwork, dimmed: the bars must stay the brightest thing on the screen. */
        ImageView art = new ImageView(this);
        art.setImageResource(R.drawable.vt3_banner);
        art.setScaleType(ImageView.ScaleType.CENTER_CROP);
        art.setAlpha(0.55f);
        art.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        root.addView(art, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        View scrim = new View(this);
        scrim.setBackgroundResource(R.drawable.bg_vt3_scrim);
        scrim.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        root.addView(scrim, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.setBackgroundColor(0x00000000);

        LinearLayout page = Ui.column(this);
        page.setPadding(Ui.dp(this, 16), Ui.dp(this, 14), Ui.dp(this, 16), Ui.dp(this, 16));
        scroll.addView(page, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(scroll, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        page.addView(shoulders());
        page.addView(brand());

        /* The menu block: weighted, so it fills the space between brand and footer and centres
         * its bars there instead of leaving one band of emptiness under the heading. */
        LinearLayout block = Ui.column(this);
        block.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        bp.topMargin = Ui.dp(this, 18);
        page.addView(block, bp);

        startBar = Ui.menuBar(this, getString(R.string.menu_start_game),
                getString(R.string.menu_hint_start), v -> startGame());
        addBar(block, startBar);
        addBar(block, Ui.menuBar(this, getString(R.string.menu_settings),
                getString(R.string.menu_hint_settings),
                v -> startActivity(new Intent(this, Settings.class))));
        addBar(block, Ui.menuBar(this, getString(R.string.menu_language),
                getString(R.string.menu_hint_language),
                v -> startActivity(new Intent(this, LanguageSelect.class))));
        /* Under Language, as the game puts its exit at the foot of the stack. */
        addBar(block, Ui.menuBar(this, getString(R.string.menu_quit),
                getString(R.string.menu_hint_quit), v -> quit()));

        TextView hint = Ui.pill(this, getString(R.string.menu_pad_hint));
        hint.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams hp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        hp.gravity = Gravity.CENTER_HORIZONTAL;
        hp.topMargin = Ui.dp(this, 18);
        page.addView(hint, hp);

        LinearLayout foot = Ui.column(this);
        foot.setBackgroundResource(R.drawable.bg_vt3_panel_dark);
        foot.setPadding(Ui.dp(this, 12), Ui.dp(this, 10), Ui.dp(this, 12), Ui.dp(this, 10));
        LinearLayout.LayoutParams fp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        fp.topMargin = Ui.dp(this, 12);
        page.addView(foot, fp);

        footer = Ui.text(this, "", 11, getColor(R.color.vt3_on_dark_soft), false);
        footer.setText(getString(R.string.menu_footer, Game.dataRoot(this).getAbsolutePath()));
        foot.addView(footer);

        TextView version = Ui.text(this, getString(R.string.menu_version, versionName()), 10,
                getColor(R.color.vt3_on_dark_soft), false);
        version.setPadding(0, Ui.dp(this, 2), 0, 0);
        foot.addView(version);

        return root;
    }

    void addBar(LinearLayout block, View bar) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        p.bottomMargin = Ui.dp(this, 7);
        block.addView(bar, p);
        final int index = bars.size();
        bars.add(bar);
        bar.setOnFocusChangeListener((v, has) -> {
            if (has) select(index);
        });
    }

    /** The shoulder caps at the two top corners, as the game draws LB and RB. */
    View shoulders() {
        LinearLayout row = Ui.rowOf(this);
        row.addView(Ui.pill(this, "LB"));
        row.addView(new View(this), new LinearLayout.LayoutParams(0, 1, 1f));
        row.addView(Ui.pill(this, "RB"));
        return row;
    }

    View brand() {
        LinearLayout col = Ui.column(this);
        LinearLayout.LayoutParams cp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        cp.topMargin = Ui.dp(this, 16);

        LinearLayout line = Ui.rowOf(this);
        TextView mark = Ui.text(this, getString(R.string.wordmark), 18,
                getColor(R.color.vt3_white), true);
        mark.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD_ITALIC));
        mark.setBackgroundResource(R.drawable.bg_wordmark);
        mark.setPadding(Ui.dp(this, 9), Ui.dp(this, 3), Ui.dp(this, 9), Ui.dp(this, 3));
        line.addView(mark);
        TextView title = Ui.title(this, getString(R.string.title));
        LinearLayout.LayoutParams tp = new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        tp.leftMargin = Ui.dp(this, 10);
        line.addView(title, tp);
        col.addView(line);

        TextView sub = Ui.subtitle(this, getString(R.string.menu_main_title));
        sub.setPadding(0, Ui.dp(this, 6), 0, 0);
        col.addView(sub);
        return col;
    }

    void select(int index) {
        if (bars.isEmpty()) return;
        selected = (index + bars.size()) % bars.size();
        for (int i = 0; i < bars.size(); i++) Ui.select(bars.get(i), i == selected);
    }

    void startGame() {
        if (!Game.installed(this)) {
            String problem = Game.problem(this);
            footer.setText(problem != null ? problem : getString(R.string.menu_not_installed));
            startActivity(new Intent(this, Installer.class));
            finish();
            return;
        }
        Game.launch(this);
    }

    /** D-pad and controller navigation, so the menu works on a handheld with no touch. */
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        if (code == KeyEvent.KEYCODE_DPAD_UP || code == KeyEvent.KEYCODE_DPAD_LEFT) {
            if (down) select(selected - 1);
            return true;
        }
        if (code == KeyEvent.KEYCODE_DPAD_DOWN || code == KeyEvent.KEYCODE_DPAD_RIGHT) {
            if (down) select(selected + 1);
            return true;
        }
        if (code == KeyEvent.KEYCODE_DPAD_CENTER || code == KeyEvent.KEYCODE_ENTER
                || code == KeyEvent.KEYCODE_BUTTON_A) {
            if (down && selected < bars.size()) bars.get(selected).performClick();
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    @Override
    public void onBackPressed() {
        /* Back on the main menu leaves the app; it does not drop into setup. */
        moveTaskToBack(true);
    }
}
