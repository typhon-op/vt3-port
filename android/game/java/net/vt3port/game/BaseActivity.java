package net.vt3port.game;

import android.app.Activity;
import android.content.Context;

/**
 * Every screen runs in the interface language chosen on the Language screen, and every screen
 * follows a change to it without the app being restarted.
 */
public class BaseActivity extends Activity {
    /** The language this screen was built with; see {@link Lang#generation()}. */
    private int localeGeneration;

    @Override
    protected void attachBaseContext(Context base) {
        localeGeneration = Lang.generation();
        super.attachBaseContext(Lang.wrap(base));
    }

    /**
     * A screen that was already alive when the language changed keeps the strings it was built
     * with: it is rebuilt here, before it is shown again. This is what the menu behind the picker
     * was missing, so picking Arabic looked like it had not taken effect until the app restarted.
     */
    @Override
    protected void onResume() {
        super.onResume();
        if (localeGeneration != Lang.generation()) recreate();
    }
}
