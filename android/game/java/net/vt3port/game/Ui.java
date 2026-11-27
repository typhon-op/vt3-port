package net.vt3port.game;

import android.content.Context;
import android.graphics.Typeface;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

/**
 * The widgets every screen is built from, so the Virtua Tennis 3 look lives in one place instead of
 * being repeated (and drifting) per activity.
 *
 * The shape of a screen: a pale silver page, translucent silver panels, angled menu bars that turn
 * orange-red when selected, charcoal text on the light plates and white/silver titles. Everything
 * here is platform Material — the APK is packaged with aapt2 + d8, so there is no AppCompat.
 */
final class Ui {
    private Ui() {}

    static int dp(Context c, float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
                c.getResources().getDisplayMetrics()));
    }

    static int color(Context c, int res) {
        return c.getColor(res);
    }

    static LinearLayout column(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    static LinearLayout rowOf(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        return l;
    }

    static TextView text(Context c, CharSequence s, float sp, int color, boolean bold) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        return t;
    }

    /** A large screen title: white/silver capitals with a soft shadow, as the game's headings. */
    static TextView title(Context c, CharSequence s) {
        TextView t = text(c, s, 26, color(c, R.color.vt3_white), true);
        t.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD_ITALIC));
        t.setAllCaps(true);
        t.setLetterSpacing(0.08f);
        t.setShadowLayer(dp(c, 2), 0, dp(c, 1), 0x66000000);
        return t;
    }

    static TextView subtitle(Context c, CharSequence s) {
        TextView t = text(c, s, 11, color(c, R.color.vt3_silver_light), true);
        t.setAllCaps(true);
        t.setLetterSpacing(0.12f);
        return t;
    }

    /** A translucent silver panel. */
    static LinearLayout panel(Context c) {
        LinearLayout l = column(c);
        l.setBackgroundResource(R.drawable.bg_panel);
        int p = dp(c, 14);
        l.setPadding(p, p, p, p);
        return l;
    }

    /**
     * One angled menu bar. The label is dark on the silver plate and white on the orange selection;
     * {@link #select} swaps both, so the selected row is unmistakable.
     */
    static FrameLayout menuBar(Context c, String label, String hint, View.OnClickListener click) {
        FrameLayout holder = new FrameLayout(c);
        holder.setBackgroundResource(R.drawable.bg_bar);
        LinearLayout inner = rowOf(c);
        inner.setPadding(dp(c, 22), dp(c, 14), dp(c, 16), dp(c, 14));
        TextView name = text(c, label, 19, color(c, R.color.vt3_ink), true);
        name.setAllCaps(true);
        name.setLetterSpacing(0.06f);
        name.setTag("label");
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        inner.addView(name, lp);
        if (hint != null && !hint.isEmpty()) {
            TextView h = text(c, hint, 11, color(c, R.color.vt3_muted), true);
            h.setAllCaps(true);
            h.setLetterSpacing(0.1f);
            h.setTag("hint");
            LinearLayout.LayoutParams hp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            hp.leftMargin = dp(c, 10);
            inner.addView(h, hp);
        }
        TextView arrow = text(c, "\u25B6", 14, color(c, R.color.vt3_muted), true);
        arrow.setTag("arrow");
        LinearLayout.LayoutParams ap = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        ap.leftMargin = dp(c, 12);
        inner.addView(arrow, ap);
        holder.addView(inner);
        holder.setClickable(true);
        holder.setFocusable(true);
        holder.setOnClickListener(click);
        return holder;
    }

    /** Silver or orange-red: the one thing that says which bar the player is on. */
    static void select(View bar, boolean on) {
        bar.setBackgroundResource(on ? R.drawable.bg_bar_selected : R.drawable.bg_bar);
        tintLabels(bar, on);
    }

    static void tintLabels(View bar, boolean on) {
        ViewGroup g = bar instanceof ViewGroup ? (ViewGroup) bar : null;
        if (g == null) return;
        for (int i = 0; i < g.getChildCount(); i++) {
            View child = g.getChildAt(i);
            if (child instanceof TextView && "label".equals(child.getTag())) {
                ((TextView) child).setTextColor(on ? 0xFFFFFFFF : 0xFF2B2723);
            } else if (child instanceof TextView && "hint".equals(child.getTag())) {
                ((TextView) child).setTextColor(on ? 0xFFFFE7D6 : 0xFF6E7278);
            } else if (child instanceof TextView && "arrow".equals(child.getTag())) {
                ((TextView) child).setTextColor(on ? 0xFFFFFFFF : 0xFF6E7278);
            } else {
                tintLabels(child, on);
            }
        }
    }

    static TextView section(Context c, CharSequence s) {
        TextView t = text(c, s, 11, color(c, R.color.vt3_orange_deep), true);
        t.setAllCaps(true);
        t.setLetterSpacing(0.14f);
        t.setPadding(0, dp(c, 16), 0, dp(c, 6));
        return t;
    }

    /** A settings row: label (and optional hint) on the left, a control on the right. */
    static LinearLayout row(Context c, String label, String hint, View control) {
        LinearLayout outer = column(c);
        outer.setBackgroundResource(R.drawable.bg_row);
        int p = dp(c, 10);
        outer.setPadding(p, p, p, p);
        LinearLayout top = rowOf(c);
        TextView name = text(c, label, 15, color(c, R.color.vt3_ink), true);
        top.addView(name, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        if (control != null) {
            LinearLayout.LayoutParams cp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            cp.leftMargin = dp(c, 12);
            top.addView(control, cp);
        }
        outer.addView(top);
        if (hint != null && !hint.isEmpty()) {
            TextView h = text(c, hint, 11, color(c, R.color.vt3_muted), false);
            h.setPadding(0, dp(c, 2), 0, 0);
            outer.addView(h);
        }
        return outer;
    }

    static Switch toggle(Context c, boolean on, android.widget.CompoundButton.OnCheckedChangeListener l) {
        Switch s = new Switch(c);
        s.setChecked(on);
        s.setButtonTintList(android.content.res.ColorStateList.valueOf(
                color(c, R.color.vt3_orange)));
        s.setOnCheckedChangeListener(l);
        return s;
    }

    static Button button(Context c, String label, View.OnClickListener click) {
        Button b = new Button(c);
        b.setText(label);
        b.setAllCaps(true);
        b.setTextSize(13);
        b.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        b.setBackgroundResource(R.drawable.bg_button_pick);
        b.setTextColor(color(c, R.color.vt3_ink));
        b.setMinHeight(dp(c, 46));
        b.setPadding(dp(c, 16), dp(c, 10), dp(c, 16), dp(c, 10));
        b.setOnClickListener(click);
        return b;
    }

    static Button primary(Context c, String label, View.OnClickListener click) {
        Button b = button(c, label, click);
        b.setBackgroundResource(R.drawable.bg_button_install);
        b.setTextColor(color(c, R.color.vt3_white));
        b.setTextSize(15);
        b.setMinHeight(dp(c, 52));
        return b;
    }

    /**
     * A value picker. A Spinner would be the obvious widget, but its dropdown is a platform dialog
     * that ignores the theme; a cycling button keeps the VT3 plate and works with a controller's
     * d-pad, which is what the settings screen is navigated with on a handheld.
     */
    static final class Choice extends Button {
        final String[] options;
        int index;
        private final Runnable onChange;

        Choice(Context c, String[] options, int index, Runnable onChange) {
            super(c);
            this.options = options;
            this.index = Math.max(0, Math.min(options.length - 1, index));
            this.onChange = onChange;
            setText(options[this.index]);
            setAllCaps(false);
            setTextSize(13);
            setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
            setTextColor(color(c, R.color.vt3_blue));
            setBackgroundResource(R.drawable.bg_tab);
            setPadding(dp(c, 12), dp(c, 8), dp(c, 12), dp(c, 8));
            setMinHeight(0);
            setMinWidth(dp(c, 110));
            setOnClickListener(v -> step(1));
        }

        void step(int dir) {
            index = (index + dir + options.length) % options.length;
            setText(options[index]);
            if (onChange != null) onChange.run();
        }

        void setIndex(int i) {
            index = Math.max(0, Math.min(options.length - 1, i));
            setText(options[index]);
        }
    }

    /** A slider with its live value on the right, in the theme's gold. */
    static LinearLayout slider(Context c, String label, String hint, int min, int max, int step,
                               int value, String unit, ValueSink sink) {
        LinearLayout outer = column(c);
        outer.setBackgroundResource(R.drawable.bg_row);
        int p = dp(c, 10);
        outer.setPadding(p, p, p, p);
        LinearLayout top = rowOf(c);
        TextView name = text(c, label, 15, color(c, R.color.vt3_ink), true);
        top.addView(name, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView shown = text(c, value + unit, 13, color(c, R.color.vt3_blue), true);
        top.addView(shown);
        outer.addView(top);
        SeekBar bar = new SeekBar(c);
        bar.setMax((max - min) / Math.max(1, step));
        bar.setProgress((value - min) / Math.max(1, step));
        bar.setProgressTintList(android.content.res.ColorStateList.valueOf(
                color(c, R.color.vt3_orange)));
        bar.setThumbTintList(android.content.res.ColorStateList.valueOf(
                color(c, R.color.vt3_orange_deep)));
        bar.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(
                color(c, R.color.vt3_line_light)));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar s, int progress, boolean fromUser) {
                int v = min + progress * step;
                shown.setText(v + unit);
                if (fromUser) sink.set(v);
            }

            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        outer.addView(bar);
        if (hint != null && !hint.isEmpty()) {
            TextView h = text(c, hint, 11, color(c, R.color.vt3_muted), false);
            h.setPadding(0, dp(c, 2), 0, 0);
            outer.addView(h);
        }
        return outer;
    }

    interface ValueSink {
        void set(int value);
    }

    /** The small gold label a read-only row shows instead of a control. */
    static TextView value(Context c, CharSequence s) {
        TextView t = text(c, s, 13, color(c, R.color.vt3_blue), true);
        t.setGravity(Gravity.END);
        return t;
    }

    /** A note under a panel: quiet, small, and never inside the tappable area. */
    static TextView note(Context c, CharSequence s) {
        TextView t = text(c, s, 11, color(c, R.color.vt3_muted), false);
        t.setLineSpacing(dp(c, 2), 1f);
        t.setPadding(0, dp(c, 8), 0, 0);
        return t;
    }

    static Spinner spinner(Context c, String[] labels, int selected) {
        Spinner s = new Spinner(c);
        ArrayAdapter<String> a = new ArrayAdapter<>(c, android.R.layout.simple_spinner_item, labels);
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        s.setAdapter(a);
        if (selected >= 0 && selected < labels.length) s.setSelection(selected);
        return s;
    }

    /** The listener a Spinner needs when its initial selection must not be treated as a choice. */
    abstract static class OnPick implements AdapterView.OnItemSelectedListener {
        abstract void pick(int index);

        @Override
        public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
            pick(position);
        }

        @Override public void onNothingSelected(AdapterView<?> parent) {}
    }
}
