package net.vt3port.game;

import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

/**
 * Preview and control test: a fixed, non-scrolling screen that draws the controls from touch.cfg at
 * their gameplay size and lets the player move them.
 *
 * It is a test harness, not the game: the geometry and the stick response are the same formulas the
 * runtime uses (touch_controls.c layout(), touch_core.c vt3_stick_apply), but this screen computes
 * them itself, because the native overlay can only draw over a presented game frame. What it
 * verifies is what a player can verify here: that the layout is where it should be, that the stick
 * reaches full travel, that the dead zone feels right, and that a swipe picks the action it says.
 * The values shown are this screen's, not a read-out of the game's live input state.
 */
public class TouchPreview extends BaseActivity {
    TouchConfig cfg;
    PreviewView view;
    TextView status, lockButton;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        cfg = TouchConfig.load(this);
        setContentView(build());
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
        page.setBackgroundColor(0xFF101215);

        LinearLayout head = Ui.rowOf(this);
        head.setBackgroundResource(R.drawable.bg_tab_selected);
        head.setPadding(Ui.dp(this, 16), Ui.dp(this, 10), Ui.dp(this, 12), Ui.dp(this, 10));
        TextView title = Ui.title(this, "PREVIEW");
        head.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        lockButton = Ui.text(this, cfg.locked() ? "UNLOCK" : "LOCK", 12, 0xFFFFFFFF, true);
        lockButton.setPadding(Ui.dp(this, 10), Ui.dp(this, 6), Ui.dp(this, 10), Ui.dp(this, 6));
        lockButton.setOnClickListener(v -> {
            cfg.setI("lock", cfg.locked() ? 0 : 1);
            cfg.save(this);
            DiagLog.i("touch", "preview: layout " + (cfg.locked() ? "locked" : "unlocked"));
            lockButton.setText(cfg.locked() ? "UNLOCK" : "LOCK");
            view.sel = -1;
            view.invalidate();
            updateHint();
        });
        head.addView(lockButton);

        TextView back = Ui.text(this, "BACK", 12, 0xFFFFFFFF, true);
        back.setPadding(Ui.dp(this, 12), Ui.dp(this, 6), Ui.dp(this, 6), Ui.dp(this, 6));
        back.setOnClickListener(v -> finish());
        head.addView(back);
        page.addView(head);

        FrameLayout stage = new FrameLayout(this);
        view = new PreviewView();
        stage.addView(view, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        page.addView(stage, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        status = Ui.text(this, "", 11, 0xFFDCE2E8, false);
        status.setPadding(Ui.dp(this, 14), Ui.dp(this, 6), Ui.dp(this, 14), Ui.dp(this, 8));
        page.addView(status);
        updateHint();
        return page;
    }

    void updateHint() {
        status.setText(cfg.locked()
                ? "Layout locked: drag disabled. Touch the controls to test the stick, buttons and swipe."
                : "Editing: drag a control to move it, drag its orange handle to resize. Changes save on release.");
    }

    /** Draws and tests the layout. The geometry mirrors runtime/src/ui/touch_controls.c layout(). */
    final class PreviewView extends View {
        float s;
        final Map<Integer, Integer> owner = new LinkedHashMap<>();   /* pointer id -> control index */
        final Map<Integer, float[]> start = new LinkedHashMap<>();   /* pointer id -> down point */
        int sel = -1;             /* control selected for editing, -1 none */
        int dragMode;             /* 0 none, 1 move, 2 resize */
        int dragId = -1;
        float grabDx, grabDy, origSize, origDist;
        int swipeDir = -1;
        boolean swipeFired;
        float lx, ly;
        int pressedMask;
        int pointerCount;

        PreviewView() {
            super(TouchPreview.this);
            setBackgroundColor(0xFF171A1F);
        }

        /* Geometry helpers, per control index in TouchConfig.CTL_IDS order. */
        float[] centre(int i) {
            float W = getWidth(), H = getHeight();
            String id = TouchConfig.CTL_IDS[i];
            float size = cfg.ctlSize(id);
            float x = cfg.ctlX(id), y = cfg.ctlY(id);
            switch (i) {
            case 0: return new float[]{ x >= 0 ? x * W : 200 * s, y >= 0 ? y * H : H - 200 * s, 168 * s * size };
            case 1: case 2: case 3: case 4: {
                float d = 152 * s * size;
                float acx = W - 275 * s, acy = H - 250 * s;
                float ox = i == 1 ? 0 : i == 2 ? 1 : i == 3 ? -1 : 0;
                float oy = i == 1 ? 1 : i == 2 ? 0 : i == 3 ? 0 : -1;
                return new float[]{ x >= 0 ? x * W : acx + ox * d, y >= 0 ? y * H : acy + oy * d, 74 * s * size };
            }
            case 5: case 6: {
                float w = 150 * s * size, h = 62 * s * size;
                float defx = i == 5 ? 40 * s + w / 2 : W - 40 * s - w / 2;
                return new float[]{ x >= 0 ? x * W : defx, y >= 0 ? y * H : 34 * s + h / 2, 0, w, h };
            }
            case 7: case 8: {
                float px = 4.2f * s * size;
                float w = (i == 7 ? 6 : 5) * 8 * px + 48 * s * size;
                float h = 66 * s * size;
                float defx = W / 2 + (i == 7 ? -140 * s : 140 * s);
                return new float[]{ x >= 0 ? x * W : defx, y >= 0 ? y * H : H - 78 * s, 0, w, h };
            }
            default:
                return new float[]{ x >= 0 ? x * W : W * 0.5f, y >= 0 ? y * H : H * 0.42f, 110 * s * size };
            }
        }

        float[] handle(int i) {
            float[] g = centre(i);
            if (g.length == 3) return new float[]{ g[0] + g[2] * 0.72f, g[1] + g[2] * 0.72f };
            return new float[]{ g[0] + g[3] * 0.5f, g[1] + g[4] * 0.5f };
        }

        /**
         * Turns a built-in position into explicit fractions before the first drag, so moving a
         * control in the Standard layout does not lose the geometry it started from. Editing a
         * named preset copies it to Custom 1 first, so the preset itself never changes.
         */
        void materialise(int i) {
            String id = TouchConfig.CTL_IDS[i];
            if (cfg.ctlX(id) >= 0 && cfg.ctlY(id) >= 0) { cfg.detachFromPreset(); return; }
            float[] g = centre(i);
            float fx = getWidth() > 0 ? g[0] / getWidth() : 0.5f;
            float fy = getHeight() > 0 ? g[1] / getHeight() : 0.5f;
            cfg.detachFromPreset();
            cfg.setF(cfg.key(id, "x"), fx);
            cfg.setF(cfg.key(id, "y"), fy);
        }

        float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

        boolean present(int i) {
            String id = TouchConfig.CTL_IDS[i];
            if (cfg.ctlRemoved(id) || !cfg.ctlVisible(id)) return false;
            if (i == 9 && !cfg.swipeEnabled()) return false;
            return true;
        }

        int pick(float x, float y) {
            int best = -1;
            float bd = 1e9f;
            for (int i = 0; i < TouchConfig.CTL_IDS.length; i++) {
                if (!present(i)) continue;
                float[] g = centre(i);
                float d;
                if (g.length == 3) {
                    float r = g[2] * (i == 0 ? 1.9f : i == 9 ? 1.0f : 1.35f);
                    d = (float) Math.hypot(x - g[0], y - g[1]);
                    if (d > r) continue;
                } else {
                    if (x < g[0] - g[3] / 2 - 14 * s || x > g[0] + g[3] / 2 + 14 * s
                            || y < g[1] - g[4] / 2 - 14 * s || y > g[1] + g[4] / 2 + 14 * s) continue;
                    d = (float) Math.hypot(x - g[0], y - g[1]);
                }
                if (owner.containsValue(i)) continue;
                if (d < bd) { bd = d; best = i; }
            }
            return best;
        }

        @Override
        protected void onDraw(android.graphics.Canvas c) {
            super.onDraw(c);
            s = getHeight() / 1080f;
            android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
            android.graphics.Paint text = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
            text.setColor(0xFFEDEFF4);
            text.setTextSize(28 * s * 1.2f);

            for (int i = 0; i < TouchConfig.CTL_IDS.length; i++) {
                if (!present(i)) continue;
                float[] g = centre(i);
                float op = cfg.ctlOpacity(TouchConfig.CTL_IDS[i]);
                boolean held = (pressedMask & (1 << i)) != 0;
                p.setStyle(android.graphics.Paint.Style.FILL);
                p.setColor(withAlpha(held ? 0x66FFFFFF : 0x22FFFFFF, op));
                if (g.length == 3) {
                    c.drawCircle(g[0], g[1], g[2], p);
                } else {
                    c.drawRoundRect(g[0] - g[3] / 2, g[1] - g[4] / 2, g[0] + g[3] / 2, g[1] + g[4] / 2,
                            g[4] / 2, g[4] / 2, p);
                }
                p.setStyle(android.graphics.Paint.Style.STROKE);
                p.setStrokeWidth(Math.max(2f, 3.2f * s));
                p.setColor(withAlpha(held ? 0xFFE2571F : 0xAAE9EEF7, op));
                if (g.length == 3) c.drawCircle(g[0], g[1], g[2], p);
                else c.drawRoundRect(g[0] - g[3] / 2, g[1] - g[4] / 2, g[0] + g[3] / 2, g[1] + g[4] / 2,
                        g[4] / 2, g[4] / 2, p);
                p.setStyle(android.graphics.Paint.Style.FILL);

                String label = i == 0 ? "STICK" : i == 9 ? "SWIPE" : TouchConfig.CTL_NAMES[i].toUpperCase(Locale.US);
                c.drawText(label, g[0] - label.length() * 7 * s * 1.2f, g[1] + 8 * s, text);
            }

            /* Knob: where the thumb is, so full travel is visible. */
            float[] st = centre(0);
            if (present(0) && owner.containsValue(0)) {
                p.setColor(0x55FFFFFF);
                c.drawCircle(lx, ly, 70 * s * cfg.ctlSize("stick"), p);
            }

            /* Selection: an orange ring and a resize handle, so it is obvious what a drag moves. */
            if (sel >= 0 && present(sel)) {
                float[] g = centre(sel);
                p.setStyle(android.graphics.Paint.Style.STROKE);
                p.setColor(0xFFE2571F);
                p.setStrokeWidth(Math.max(3f, 4f * s));
                if (g.length == 3) c.drawCircle(g[0], g[1], g[2] + 8 * s, p);
                else c.drawRoundRect(g[0] - g[3] / 2 - 8 * s, g[1] - g[4] / 2 - 8 * s,
                        g[0] + g[3] / 2 + 8 * s, g[1] + g[4] / 2 + 8 * s, g[4] / 2, g[4] / 2, p);
                p.setStyle(android.graphics.Paint.Style.FILL);
                float[] h = handle(sel);
                c.drawCircle(h[0], h[1], 18 * s, p);
            }

            /* Diagnostics block. */
            text.setTextSize(22 * s * 1.2f);
            text.setColor(0xFF9FE0A8);
            float y = 40 * s * 1.2f;
            String[] lines = {
                    "Mode: " + (cfg.locked() ? "layout locked" : "layout unlocked"),
                    "Layout: " + TouchConfig.LAYOUT_NAMES[TouchControls.indexOf(TouchConfig.LAYOUT_IDS, cfg.layout())],
                    "Stick: " + TouchConfig.PROFILE_NAMES[TouchControls.indexOf(TouchConfig.PROFILE_IDS, cfg.profile())],
                    String.format(Locale.US, "Analog X %+.2f  Y %+.2f", lx, ly),
                    "Buttons: " + maskText(pressedMask),
                    "Pointers: " + pointerCount,
                    "Swipe: " + (swipeDir < 0 ? "-" : TouchConfig.DIRS[swipeDir].toUpperCase(Locale.US))
                            + (swipeFired ? " -> " + cfg.s("swipe.map." + TouchConfig.DIRS[swipeDir], "?").toUpperCase(Locale.US) : ""),
            };
            for (String line : lines) {
                c.drawText(line, 24 * s, y, text);
                y += 30 * s * 1.2f;
            }
            if (status != null) status.setText("Analog/d-pad mode in the game is chosen by the runtime from the "
                    + "game's own state; this screen always tests the analog stick.");
        }

        String maskText(int mask) {
            StringBuilder b = new StringBuilder();
            for (int i = 1; i < 9; i++) if ((mask & (1 << i)) != 0) b.append(TouchConfig.CTL_NAMES[i]).append(' ');
            return b.length() == 0 ? "-" : b.toString().trim();
        }

        int withAlpha(int colour, float op) {
            int a = (int) (((colour >>> 24) & 0xFF) * op);
            return (a << 24) | (colour & 0xFFFFFF);
        }

        /* The stick response, same arithmetic as vt3_stick_apply. */
        void stickValues(float dx, float dy, float r, float[] out) {
            String prof = "p." + cfg.profile() + ".";
            float dz = cfg.f(prof + "deadzone", 0.14f);
            float sens = cfg.f(prof + "sensitivity", 1f);
            float range = cfg.f(prof + "range", 1f);
            boolean prog = cfg.i(prof + "progressive", 0) != 0;
            boolean invx = cfg.i(prof + "invertx", 0) != 0;
            boolean invy = cfg.i(prof + "inverty", 0) != 0;
            float lim = r * range;
            float d = (float) Math.hypot(dx, dy);
            if (d > lim && d > 0) { dx = dx * lim / d; dy = dy * lim / d; }
            float ax = dx / lim, ay = dy / lim;
            float mag = (float) Math.hypot(ax, ay);
            if (mag <= dz) { ax = 0; ay = 0; }
            else { float k = (mag - dz) / (1 - dz) / mag; ax *= k; ay *= k; }
            if (prog) { float m = (float) Math.hypot(ax, ay); ax *= m; ay *= m; }
            ax *= sens;
            ay *= sens;
            if (invx) ax = -ax;
            if (invy) ay = -ay;
            out[0] = Math.max(-1f, Math.min(1f, ax));
            out[1] = Math.max(-1f, Math.min(1f, ay));
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            pointerCount = e.getPointerCount();
            int action = e.getActionMasked();
            int idx = e.getActionIndex();
            int id = e.getPointerId(idx);
            if (s <= 0) s = getHeight() / 1080f;

            /* Unlocked: the canvas is the editor. Locked: it is the control test. */
            if (!cfg.locked()) {
                switch (action) {
                case MotionEvent.ACTION_DOWN: {
                    float x = e.getX(idx), y = e.getY(idx);
                    for (int i = 0; i < TouchConfig.CTL_IDS.length; i++) {
                        if (!present(i)) continue;
                        float[] h = handle(i);
                        if (Math.hypot(x - h[0], y - h[1]) <= 34 * s) {
                            sel = i;
                            dragMode = 2;
                            dragId = id;
                            float[] g = centre(i);
                            origDist = (float) Math.hypot(x - g[0], y - g[1]);
                            origSize = cfg.ctlSize(TouchConfig.CTL_IDS[i]);
                            materialise(i);
                            invalidate();
                            return true;
                        }
                    }
                    int ctl = pick(x, y);
                    if (ctl >= 0) {
                        sel = ctl;
                        dragMode = 1;
                        dragId = id;
                        float[] g = centre(ctl);
                        grabDx = x - g[0];
                        grabDy = y - g[1];
                        materialise(ctl);
                        invalidate();
                        return true;
                    }
                    sel = -1;
                    invalidate();
                    return true;
                }
                case MotionEvent.ACTION_MOVE: {
                    if (dragMode == 0 || sel < 0) return true;
                    int pi = e.findPointerIndex(dragId);
                    if (pi < 0) return true;
                    float x = e.getX(pi), y = e.getY(pi);
                    String cid = TouchConfig.CTL_IDS[sel];
                    float[] g = centre(sel);
                    if (dragMode == 1) {
                        cfg.setF(cfg.key(cid, "x"), clamp((x - grabDx) / getWidth(), 0.03f, 0.97f));
                        cfg.setF(cfg.key(cid, "y"), clamp((y - grabDy) / getHeight(), 0.03f, 0.97f));
                    } else {
                        float d = (float) Math.hypot(x - g[0], y - g[1]);
                        float ns = origDist > 4 ? origSize * d / origDist : origSize;
                        cfg.setF(cfg.key(cid, "size"), clamp(ns, 0.5f, 2.5f));
                    }
                    invalidate();
                    return true;
                }
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_POINTER_UP:
                case MotionEvent.ACTION_CANCEL:
                    if (dragMode != 0) {
                        dragMode = 0;
                        dragId = -1;
                        boolean ok = cfg.save(TouchPreview.this);
                        DiagLog.i("touch", "preview: control " + (sel >= 0 ? TouchConfig.CTL_IDS[sel] : "?")
                                + (ok ? " saved" : " NOT saved"));
                    }
                    invalidate();
                    return true;
                default:
                    return true;
                }
            }

            switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                float x = e.getX(idx), y = e.getY(idx);
                int ctl = pick(x, y);
                if (ctl >= 0) {
                    owner.put(id, ctl);
                    start.put(id, new float[]{ x, y });
                    if (ctl == 9) { swipeDir = -1; swipeFired = false; }
                }
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                pressedMask = 0;
                for (int pi = 0; pi < e.getPointerCount(); pi++) {
                    int pid = e.getPointerId(pi);
                    Integer ctl = owner.get(pid);
                    if (ctl == null) continue;
                    pressedMask |= 1 << ctl;
                    float x = e.getX(pi), y = e.getY(pi);
                    if (ctl == 0) {
                        float[] g = centre(0);
                        float[] v = new float[2];
                        stickValues(x - g[0], y - g[1], g[2], v);
                        lx = v[0];
                        ly = v[1];
                    } else if (ctl == 9) {
                        float[] st0 = start.get(pid);
                        float dx = x - st0[0], dy = y - st0[1];
                        float min = centre(9)[2] * cfg.f("swipe.min_dist", 0.18f) * 2f;
                        if (swipeDir < 0 && Math.max(Math.abs(dx), Math.abs(dy)) >= min) {
                            swipeDir = Math.abs(dx) > Math.abs(dy)
                                    ? (dx < 0 ? 2 : 3) : (dy < 0 ? 0 : 1);
                            swipeFired = true;
                        }
                    }
                }
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                Integer ctl = owner.remove(id);
                start.remove(id);
                if (ctl != null && ctl == 0) { lx = 0; ly = 0; }
                if (ctl != null && ctl == 9) { swipeDir = -1; swipeFired = false; }
                if (action == MotionEvent.ACTION_CANCEL) { owner.clear(); start.clear(); pressedMask = 0; lx = 0; ly = 0; }
                pressedMask = 0;
                for (Integer c : owner.values()) pressedMask |= 1 << c;
                break;
            }
            default:
                break;
            }
            invalidate();
            return true;
        }
    }
}
