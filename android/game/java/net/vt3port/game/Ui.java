package net.vt3port.game;

import android.content.Context;
import android.graphics.Typeface;
import android.text.TextUtils;
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
 * The shape of a screen now follows the game's own menus: a dark art ground, red-orange slanted
 * bars with white italic capitals, an orange heading band and thin striped rows for option lists.
 * Everything here is platform Material -- the APK is packaged with aapt2 + d8, so there is no
 * AppCompat.
 *
 * Text is kept to one line per row: a label either fits or ends in an ellipsis, so a long
 * translation cannot push a control out of its lane or turn one row into three.
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

    static boolean rtl(Context c) {
        return c.getResources().getConfiguration().getLayoutDirection()
                == View.LAYOUT_DIRECTION_RTL;
    }

    /**
     * Letter spacing, where it belongs. Arabic joins its letters, and tracking pulls the joins
     * apart into separate shapes, so a right-to-left screen keeps the script's own spacing.
     */
    static void tracking(Context c, TextView t, float v) {
        if (!rtl(c)) t.setLetterSpacing(v);
    }

    /** One line, ellipsised: the label of a row never wraps into a second lane. */
    static void oneLine(TextView t) {
        t.setSingleLine(true);
        t.setEllipsize(TextUtils.TruncateAt.END);
    }

    /** A large screen title: white/silver capitals with a soft shadow, as the game's headings. */
    static TextView title(Context c, CharSequence s) {
        TextView t = text(c, s, 26, color(c, R.color.vt3_white), true);
        t.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD_ITALIC));
        t.setAllCaps(true);
        tracking(c, t, 0.08f);
        t.setShadowLayer(dp(c, 2), 0, dp(c, 1), 0x99000000);
        oneLine(t);
        return t;
    }

    static TextView subtitle(Context c, CharSequence s) {
        TextView t = text(c, s, 11, color(c, R.color.vt3_on_dark_soft), true);
        t.setAllCaps(true);
        tracking(c, t, 0.16f);
        return t;
    }

    /** A shoulder-button cap or a legend: a dark rounded pill with white capitals. */
    static TextView pill(Context c, CharSequence s) {
        TextView t = text(c, s, 11, color(c, R.color.vt3_white), true);
        t.setAllCaps(true);
        tracking(c, t, 0.14f);
        t.setBackgroundResource(R.drawable.bg_vt3_pill);
        t.setPadding(dp(c, 12), dp(c, 5), dp(c, 12), dp(c, 5));
        return t;
    }

    /** A dark translucent panel, behind lists and notes on the menu screens. */
    static LinearLayout panel(Context c) {
        LinearLayout l = column(c);
        l.setBackgroundResource(R.drawable.bg_vt3_panel_dark);
        int p = dp(c, 10);
        l.setPadding(p, p, p, p);
        return l;
    }

    /**
     * One angled menu bar, drawn as a parallelogram like the game's own rows: deep red when the
     * player is not on it, lit orange-red when they are ({@link #select}). White italic capitals,
     * with the short hint at the right in the muted tone.
     */
    static FrameLayout menuBar(Context c, String label, String hint, View.OnClickListener click) {
        FrameLayout holder = new FrameLayout(c);
        holder.setBackgroundResource(R.drawable.bg_vt3_slant);
        LinearLayout inner = rowOf(c);
        inner.setPadding(dp(c, 26), dp(c, 13), dp(c, 22), dp(c, 13));
        TextView name = text(c, label, 20, color(c, R.color.vt3_white), true);
        name.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD_ITALIC));
        name.setAllCaps(true);
        tracking(c, name, 0.06f);
        name.setShadowLayer(dp(c, 2), 0, dp(c, 1), 0x88000000);
        oneLine(name);
        name.setTag("label");
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        inner.addView(name, lp);
        if (hint != null && !hint.isEmpty()) {
            TextView h = text(c, hint, 10, color(c, R.color.vt3_on_dark_soft), true);
            h.setAllCaps(true);
            tracking(c, h, 0.1f);
            oneLine(h);
            h.setTag("hint");
            LinearLayout.LayoutParams hp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            hp.leftMargin = dp(c, 12);
            inner.addView(h, hp);
        }
        holder.addView(inner);
        holder.setClickable(true);
        holder.setFocusable(true);
        holder.setOnClickListener(click);
        return holder;
    }

    /** Deep red or lit orange-red: the one thing that says which bar the player is on. */
    static void select(View bar, boolean on) {
        bar.setBackgroundResource(on ? R.drawable.bg_vt3_slant_on : R.drawable.bg_vt3_slant);
        tintLabels(bar, on);
    }

    static void tintLabels(View bar, boolean on) {
        ViewGroup g = bar instanceof ViewGroup ? (ViewGroup) bar : null;
        if (g == null) return;
        for (int i = 0; i < g.getChildCount(); i++) {
            View child = g.getChildAt(i);
            if (child instanceof TextView && "label".equals(child.getTag())) {
                ((TextView) child).setTextColor(on ? 0xFFFFFFFF : 0xFFF7DFD2);
            } else if (child instanceof TextView && "hint".equals(child.getTag())) {
                ((TextView) child).setTextColor(on ? 0xFFFFF0E4 : 0xFFFFC2A4);
            } else if (child instanceof TextView && "arrow".equals(child.getTag())) {
                ((TextView) child).setTextColor(on ? 0xFFFFFFFF : 0xFFFFC2A4);
            } else {
                tintLabels(child, on);
            }
        }
    }

    /** A category heading on the dark ground: gold capitals, as above the option lists. */
    static TextView section(Context c, CharSequence s) {
        TextView t = text(c, s, 11, color(c, R.color.vt3_gold_light), true);
        t.setAllCaps(true);
        tracking(c, t, 0.14f);
        t.setPadding(0, dp(c, 16), 0, dp(c, 6));
        return t;
    }

    /** A settings row: label (and optional hint) on the left, a control on the right, one lane. */
    static LinearLayout row(Context c, String label, String hint, View control) {
        LinearLayout outer = column(c);
        outer.setBackgroundResource(R.drawable.bg_vt3_band);
        int p = dp(c, 10);
        outer.setPadding(p, p, p, p);
        LinearLayout top = rowOf(c);
        TextView name = text(c, label, 15, color(c, R.color.vt3_band_text), true);
        oneLine(name);
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
            TextView h = text(c, hint, 11, color(c, R.color.vt3_on_dark_soft), false);
            h.setPadding(0, dp(c, 3), 0, 0);
            h.setLineSpacing(dp(c, 1), 1f);
            outer.addView(h);
        }
        return outer;
    }

    static Switch toggle(Context c, boolean on, android.widget.CompoundButton.OnCheckedChangeListener l) {
        Switch s = new Switch(c);
        s.setChecked(on);
        s.setThumbTintList(android.content.res.ColorStateList.valueOf(
                color(c, R.color.vt3_orange_light)));
        s.setTrackTintList(android.content.res.ColorStateList.valueOf(0x59FFFFFF));
        s.setOnCheckedChangeListener(l);
        return s;
    }

    static Button button(Context c, String label, View.OnClickListener click) {
        Button b = new Button(c);
        b.setText(label);
        b.setAllCaps(true);
        b.setTextSize(13);
        b.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        b.setBackgroundResource(R.drawable.bg_vt3_pill);
        b.setTextColor(color(c, R.color.vt3_white));
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
     * that ignores the theme; a cycling button keeps the game's plate and works with a controller's
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
            setTextColor(color(c, R.color.vt3_gold_light));
            setBackgroundResource(R.drawable.bg_vt3_pill);
            setPadding(dp(c, 12), dp(c, 7), dp(c, 12), dp(c, 7));
            setMinHeight(0);
            setMinWidth(dp(c, 96));
            setSingleLine(true);
            setEllipsize(TextUtils.TruncateAt.END);
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
        outer.setBackgroundResource(R.drawable.bg_vt3_band);
        int p = dp(c, 10);
        outer.setPadding(p, p, p, p);
        LinearLayout top = rowOf(c);
        TextView name = text(c, label, 15, color(c, R.color.vt3_band_text), true);
        oneLine(name);
        top.addView(name, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView shown = text(c, value + unit, 13, color(c, R.color.vt3_gold_light), true);
        top.addView(shown);
        outer.addView(top);
        SeekBar bar = new SeekBar(c);
        bar.setMax((max - min) / Math.max(1, step));
        bar.setProgress((value - min) / Math.max(1, step));
        bar.setProgressTintList(android.content.res.ColorStateList.valueOf(
                color(c, R.color.vt3_orange)));
        bar.setThumbTintList(android.content.res.ColorStateList.valueOf(
                color(c, R.color.vt3_orange_light)));
        bar.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(0x4DFFFFFF));
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
            TextView h = text(c, hint, 11, color(c, R.color.vt3_on_dark_soft), false);
            h.setPadding(0, dp(c, 2), 0, 0);
            h.setLineSpacing(dp(c, 1), 1f);
            outer.addView(h);
        }
        return outer;
    }

    interface ValueSink {
        void set(int value);
    }

    /** The small gold label a read-only row shows instead of a control. */
    static TextView value(Context c, CharSequence s) {
        TextView t = text(c, s, 13, color(c, R.color.vt3_gold_light), true);
        t.setGravity(Gravity.END);
        t.setSingleLine(true);
        t.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        t.setMaxWidth(dp(c, 190));
        return t;
    }

    /** A note under a panel: quiet, small, and never inside the tappable area. */
    static TextView note(Context c, CharSequence s) {
        TextView t = text(c, s, 11, color(c, R.color.vt3_on_dark_soft), false);
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
