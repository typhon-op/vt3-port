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

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * The menu after setup: Start Game, Settings, Language.
 *
 * The install is re-checked every time the screen comes back, so a folder that was moved or a card
 * that was removed sends the player to setup instead of into a game that cannot start. The chosen
 * game folder is not re-picked: it lives in the install markers (see {@link Game}).
 *
 * Layout: the banner at the top, the three bars centred in whatever space is left, the install line
 * at the foot. The bars take their height from their text and the block is weighted, so a tall
 * phone does not get a band of empty screen above the menu and a short one scrolls -- no fixed
 * offsets and no negative margins.
 */
public class MainMenu extends BaseActivity {
    final List<View> bars = new ArrayList<>();
    int selected;
    TextView footer;
    View startBar;

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
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(getColor(R.color.vt3_page));
        scroll.setVerticalScrollBarEnabled(false);

        LinearLayout page = Ui.column(this);
        page.setPadding(Ui.dp(this, 16), Ui.dp(this, 16), Ui.dp(this, 16), Ui.dp(this, 18));
        scroll.addView(page, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        page.addView(header());

        /* The menu block: weighted, so it fills the space between banner and footer and centres
         * its bars there instead of leaving one band of emptiness under the banner. */
        LinearLayout block = Ui.column(this);
        block.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        bp.topMargin = Ui.dp(this, 14);
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

        /* Controller hint, in the game's own voice. */
        TextView hint = Ui.text(this, getString(R.string.menu_pad_hint), 11,
                getColor(R.color.vt3_muted), true);
        hint.setAllCaps(true);
        hint.setLetterSpacing(0.1f);
        hint.setGravity(Gravity.CENTER);
        hint.setPadding(0, Ui.dp(this, 16), 0, 0);
        block.addView(hint);

        LinearLayout foot = Ui.column(this);
        foot.setBackgroundResource(R.drawable.bg_ticker);
        foot.setPadding(Ui.dp(this, 12), Ui.dp(this, 10), Ui.dp(this, 12), Ui.dp(this, 10));
        LinearLayout.LayoutParams fp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        fp.topMargin = Ui.dp(this, 12);
        page.addView(foot, fp);

        footer = Ui.text(this, "", 11, getColor(R.color.vt3_ink_soft), false);
        footer.setText(getString(R.string.menu_footer, Game.dataRoot(this).getAbsolutePath()));
        foot.addView(footer);

        TextView version = Ui.text(this, getString(R.string.menu_version, versionName()), 10,
                getColor(R.color.vt3_muted), false);
        version.setPadding(0, Ui.dp(this, 2), 0, 0);
        foot.addView(version);

        return scroll;
    }

    void addBar(LinearLayout block, View bar) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        p.bottomMargin = Ui.dp(this, 10);
        block.addView(bar, p);
        final int index = bars.size();
        bars.add(bar);
        bar.setOnFocusChangeListener((v, has) -> {
            if (has) select(index);
        });
    }

    View header() {
        FrameLayout banner = new FrameLayout(this);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(this, 196));
        banner.setLayoutParams(lp);

        ImageView img = new ImageView(this);
        img.setImageResource(R.drawable.vt3_banner);
        img.setScaleType(ImageView.ScaleType.CENTER_CROP);
        img.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        banner.addView(img, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        View scrim = new View(this);
        scrim.setBackgroundResource(R.drawable.bg_scrim_bottom);
        banner.addView(scrim, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        LinearLayout plate = Ui.column(this);
        plate.setPadding(Ui.dp(this, 14), Ui.dp(this, 10), Ui.dp(this, 14), Ui.dp(this, 12));
        FrameLayout.LayoutParams pp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        pp.gravity = Gravity.BOTTOM;
        banner.addView(plate, pp);

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
        plate.addView(line);
        plate.addView(Ui.subtitle(this, getString(R.string.menu_main_title)));

        return banner;
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
