package net.vt3port.game;

import android.os.Bundle;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * The launcher's language. The game's own text comes from the installed disc (the European release
 * carries English, French, German, Spanish and Italian), so this changes these screens only, and
 * says so rather than implying it re-dubs the game.
 *
 * Look: the same dark ground and red slanted bars as the main menu, with the current language lit
 * so the choice is visible before it is made.
 */
public class LanguageSelect extends BaseActivity {
    final List<View> bars = new ArrayList<>();
    int selected;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(build());
        select(Lang.index(this));
    }

    View build() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundResource(R.drawable.bg_vt3_ground);
        scroll.setVerticalScrollBarEnabled(false);

        LinearLayout page = Ui.column(this);
        page.setPadding(Ui.dp(this, 16), Ui.dp(this, 16), Ui.dp(this, 16), Ui.dp(this, 18));
        scroll.addView(page, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout head = Ui.rowOf(this);
        head.setBackgroundResource(R.drawable.bg_vt3_title);
        head.setPadding(Ui.dp(this, 16), Ui.dp(this, 12), Ui.dp(this, 12), Ui.dp(this, 12));
        head.addView(Ui.title(this, getString(R.string.language_title)),
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView back = Ui.text(this, getString(R.string.settings_close), 12, 0xFFFFFFFF, true);
        back.setAllCaps(true);
        Ui.tracking(this, back, 0.1f);
        back.setBackgroundResource(R.drawable.bg_vt3_pill);
        back.setPadding(Ui.dp(this, 12), Ui.dp(this, 6), Ui.dp(this, 12), Ui.dp(this, 6));
        back.setOnClickListener(v -> finish());
        head.addView(back);
        page.addView(head);

        TextView hint = Ui.text(this, getString(R.string.language_hint), 12,
                getColor(R.color.vt3_on_dark_soft), false);
        hint.setLineSpacing(Ui.dp(this, 2), 1f);
        hint.setPadding(0, Ui.dp(this, 12), 0, Ui.dp(this, 12));
        page.addView(hint);

        for (int i = 0; i < Lang.CODES.length; i++) {
            final int index = i;
            View bar = Ui.menuBar(this, Lang.NAMES[i], Lang.CODES[i].equals("system") ? "" : Lang.CODES[i],
                    v -> choose(index));
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            p.bottomMargin = Ui.dp(this, 7);
            page.addView(bar, p);
            bars.add(bar);
        }

        TextView note = Ui.note(this, getString(R.string.language_note));
        page.addView(note);
        return scroll;
    }

    void choose(int index) {
        select(index);
        Lang.set(this, Lang.CODES[index]);
        DiagLog.i("app", "language set to " + Lang.CODES[index]);
        /* This screen is rebuilt now; the screens already behind it rebuild themselves in
         * BaseActivity.onResume, so the menu is in the new language as soon as it is shown. The
         * choice is a marker, so nothing is lost. */
        recreate();
    }

    void select(int index) {
        if (bars.isEmpty()) return;
        selected = Math.max(0, Math.min(bars.size() - 1, index));
        for (int i = 0; i < bars.size(); i++) Ui.select(bars.get(i), i == selected);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        if (code == KeyEvent.KEYCODE_DPAD_UP) { if (down) select(selected - 1); return true; }
        if (code == KeyEvent.KEYCODE_DPAD_DOWN) { if (down) select(selected + 1); return true; }
        if (code == KeyEvent.KEYCODE_DPAD_CENTER || code == KeyEvent.KEYCODE_ENTER
                || code == KeyEvent.KEYCODE_BUTTON_A) {
            if (down && selected < bars.size()) bars.get(selected).performClick();
            return true;
        }
        if (code == KeyEvent.KEYCODE_BUTTON_B) { if (down) finish(); return true; }
        return super.dispatchKeyEvent(e);
    }
}
