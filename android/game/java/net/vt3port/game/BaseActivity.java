package net.vt3port.game;

import android.app.Activity;
import android.content.Context;

/** Every screen runs in the interface language chosen on the Language screen. */
public class BaseActivity extends Activity {
    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(Lang.wrap(base));
    }
}
