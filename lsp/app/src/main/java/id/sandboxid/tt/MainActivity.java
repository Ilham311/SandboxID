package id.sandboxid.tt;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.text.DateFormat;
import java.util.Date;
import java.util.Map;

/**
 * The module's own screen — the "one click" this variant runs on.
 *
 * <p>The Zygisk module's one-click lives in the manager's Action button, which
 * runs a root script. An LSPosed module has no root and no manager hook, so the
 * button is <i>here</i>, in the module's own launcher activity: it generates a
 * complete new persona ({@link PersonaGenerator}) and stores it where the hooks
 * in the target process read it back ({@link IdentityStore}).
 *
 * <p>After rotating, the target app must be killed for the new persona to apply
 * — hooks run at process start, and a running process has already read the old
 * one. The root variant does this with {@code pm clear}; without root we can
 * only ask the user to swipe the app away, which is what the button explains.
 */
public class MainActivity extends Activity {

    private static final String TARGET = SandboxIdTT.TARGET;

    private TextView status;
    private TextView personaView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        status = new TextView(this);
        personaView = new TextView(this);
        personaView.setTypeface(android.graphics.Typeface.MONOSPACE);
        personaView.setTextSize(12);

        Button rotate = new Button(this);
        rotate.setText("Generate new device persona");
        rotate.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                rotate();
            }
        });

        Button openTikTok = new Button(this);
        openTikTok.setText("Open TikTok (swipe it away from recents first)");
        openTikTok.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Intent i = getPackageManager()
                        .getLaunchIntentForPackage(TARGET);
                if (i != null) {
                    i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    startActivity(i);
                } else {
                    status.setText("TikTok is not installed.");
                }
            }
        });

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);
        root.addView(status);
        root.addView(spacer(8));
        root.addView(rotate);
        root.addView(openTikTok);
        root.addView(spacer(16));
        root.addView(personaView);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);

        render();
    }

    private void rotate() {
        Map<String, String> id = PersonaGenerator.generate(this);
        if (id == null || id.isEmpty()) {
            status.setText("Failed: the bundled device pool could not be read. "
                    + "This is a packaging bug — please report it.");
            return;
        }
        IdentityStore.save(this, id);
        render();
        status.setText("New persona generated. Kill TikTok (swipe it from the "
                + "recents list) and reopen it for the new device to apply.");
    }

    private void render() {
        SharedPreferences sp = getSharedPreferences(IdentityStore.PREFS, MODE_PRIVATE);
        String blob = sp.getString(IdentityStore.KEY_PERSONA, null);
        if (blob == null) {
            personaView.setText("No persona yet. Press the button above to "
                    + "generate one.");
            status.setText("This module is self-contained: it needs no root and "
                    + "no other module. Enable it in LSPosed Manager with TikTok "
                    + "in scope, then generate a persona here.");
            return;
        }
        StringBuilder sb = new StringBuilder();
        sb.append("Generated ")
          .append(DateFormat.getDateTimeInstance().format(new Date()))
          .append("\n\n");
        for (String line : blob.split("\n", -1)) {
            if (line.isEmpty()) continue;
            sb.append(line).append('\n');
        }
        personaView.setText(sb.toString());
        status.setText("Persona ready. Kill and reopen TikTok to apply it.");
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private View spacer(int heightDp) {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(heightDp)));
        return v;
    }
}
