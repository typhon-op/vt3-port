package net.vt3port.game;

import android.app.LocaleManager;
import android.content.Context;
import android.content.res.Configuration;
import android.os.Build;
import android.os.LocaleList;

import java.util.Locale;

/**
 * The launcher's own language. The game's text comes from the disc the player installed (the
 * European release ships English, French, German, Spanish and Italian), so this only chooses the
 * language of these screens: setup, the main menu, settings and the diagnostics.
 *
 * The choice is a marker file, read in attachBaseContext, so a screen that is opened after the
 * choice is already in it. A screen that is already alive is rebuilt by {@link #generation()}:
 * picking a language used to leave the menu underneath the picker in the old language until the
 * app was restarted.
 */
final class Lang {
    static final String[] CODES = { "system", "en", "fr", "de", "es", "it", "ar" };
    static final String[] NAMES = { "System language", "English", "Fran\u00e7ais", "Deutsch",
            "Espa\u00f1ol", "Italiano", "\u0627\u0644\u0639\u0631\u0628\u064a\u0629" };

    /**
     * Counts the changes made in this process. A screen records it in attachBaseContext and
     * compares it in onResume, so every live screen rebuilds itself the moment the choice changes.
     */
    private static int generation;

    private Lang() {}

    static int generation() {
        return generation;
    }

    static String code(Context c) {
        String s = Markers.read(Game.root(c), "lang", "system");
        for (String code : CODES) if (code.equals(s)) return code;
        return "system";
    }

    static void set(Context c, String code) {
        Markers.write(Game.root(c), "lang", code);
        generation++;
        applyAppLocales(c, code);
        DiagLog.i("app", "interface language " + code);
    }

    static int index(Context c) {
        String s = code(c);
        for (int i = 0; i < CODES.length; i++) if (CODES[i].equals(s)) return i;
        return 0;
    }

    /**
     * Android 13 keeps the app's language itself: LocaleManager applies the choice to the whole
     * app at once (the platform rebuilds every activity, this one and the menu behind it) and keeps
     * it across restarts. Older releases have no such API; there the generation is what rebuilds
     * the screens.
     */
    private static void applyAppLocales(Context c, String code) {
        if (Build.VERSION.SDK_INT < 33) return;
        try {
            LocaleManager lm = c.getSystemService(LocaleManager.class);
            if (lm == null) return;
            lm.setApplicationLocales("system".equals(code)
                    ? LocaleList.getEmptyLocaleList()
                    : new LocaleList(new Locale(code)));
        } catch (RuntimeException e) {
            /* Not fatal: the marker and the generation still rebuild every screen. */
            DiagLog.w("app", "the system would not take the language: " + e);
        }
    }

    /** The context an activity should run with: the chosen locale, or the system's own. */
    static Context wrap(Context base) {
        String code = code(base);
        if ("system".equals(code)) return base;
        Locale locale = new Locale(code);
        Locale.setDefault(locale);
        Configuration cfg = new Configuration(base.getResources().getConfiguration());
        cfg.setLocale(locale);
        cfg.setLocales(new LocaleList(locale));
        return base.createConfigurationContext(cfg);
    }
}
