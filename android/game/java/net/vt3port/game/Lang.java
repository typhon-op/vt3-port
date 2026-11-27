package net.vt3port.game;

import android.content.Context;
import android.content.res.Configuration;
import android.os.LocaleList;

import java.util.Locale;

/**
 * The launcher's own language. The game's text comes from the disc the player installed (the
 * European release ships English, French, German, Spanish and Italian), so this only chooses the
 * language of these screens: setup, the main menu, settings and the diagnostics.
 *
 * The choice is a marker file, read once per activity in attachBaseContext, so every screen picks
 * it up without a restart.
 */
final class Lang {
    static final String[] CODES = { "system", "en", "fr", "de", "es", "it" };
    static final String[] NAMES = { "System language", "English", "Fran\u00e7ais", "Deutsch",
            "Espa\u00f1ol", "Italiano" };

    private Lang() {}

    static String code(Context c) {
        String s = Markers.read(Game.root(c), "lang", "system");
        for (String code : CODES) if (code.equals(s)) return code;
        return "system";
    }

    static void set(Context c, String code) {
        Markers.write(Game.root(c), "lang", code);
        DiagLog.i("app", "interface language " + code);
    }

    static int index(Context c) {
        String s = code(c);
        for (int i = 0; i < CODES.length; i++) if (CODES[i].equals(s)) return i;
        return 0;
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
