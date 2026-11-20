package net.vt3port.game;

import android.app.Activity;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;

/* Reached from the launcher icon's long-press shortcut. Each setting is a marker file beside the
 * game data that android_main reads at start-up (a vt3.env entry for the same variable wins). */
public class Settings extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        File fps60 = new File(getExternalFilesDir(null), "fps60");
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        box.setBackgroundColor(getColor(R.color.vt3_page));
        box.setPadding(48, 48, 48, 48);
        TextView title = new TextView(this);
        title.setText(R.string.settings_title);
        title.setTextSize(24);
        title.setTextColor(getColor(R.color.vt3_text));
        title.setTextAlignment(TextView.TEXT_ALIGNMENT_CENTER);
        box.addView(title);
        Switch saver = new Switch(this);
        saver.setText(R.string.settings_saver);
        saver.setTextColor(getColor(R.color.vt3_text));
        saver.setChecked(fps60.exists());
        saver.setOnCheckedChangeListener((v, on) -> {
            try {
                if (on) fps60.createNewFile(); else fps60.delete();
            } catch (IOException e) {
                v.setChecked(fps60.exists());
            }
        });
        box.addView(saver);
        TextView note = new TextView(this);
        note.setText(R.string.settings_note);
        note.setTextColor(getColor(R.color.vt3_muted));
        box.addView(note);
        setContentView(box);
        saver.requestFocus();
    }
}
