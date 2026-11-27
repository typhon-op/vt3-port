package net.vt3port.game;

import android.os.Bundle;
import android.view.Gravity;
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
        scroll.setBackgroundColor(getColor(R.color.vt3_page));
        scroll.setVerticalScrollBarEnabled(false);

        LinearLayout page = Ui.column(this);
        page.setPadding(Ui.dp(this, 16), Ui.dp(this, 18), Ui.dp(this, 16), Ui.dp(this, 18));
        scroll.addView(page, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout head = Ui.column(this);
        head.setBackgroundResource(R.drawable.bg_tab_selected);
        head.setPadding(Ui.dp(this, 16), Ui.dp(this, 12), Ui.dp(this, 16), Ui.dp(this, 12));
        head.addView(Ui.title(this, getString(R.string.language_title)));
        page.addView(head);

        TextView hint = Ui.text(this, getString(R.string.language_hint), 12,
                getColor(R.color.vt3_ink_soft), false);
        hint.setLineSpacing(Ui.dp(this, 2), 1f);
        hint.setPadding(0, Ui.dp(this, 10), 0, Ui.dp(this, 10));
        page.addView(hint);

        LinearLayout list = Ui.panel(this);
        page.addView(list);

        for (int i = 0; i < Lang.CODES.length; i++) {
            final int index = i;
            View bar = Ui.menuBar(this, Lang.NAMES[i], Lang.CODES[i].equals("system") ? "" : Lang.CODES[i],
                    v -> choose(index));
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            p.bottomMargin = Ui.dp(this, 8);
            list.addView(bar, p);
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
        /* The whole stack is recreated so every screen, including this one, is in the new
         * language; the choice is a marker, so nothing is lost. */
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
