package id.sandboxid.tt;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
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
 * runs a root script. An LSPosed module has no manager hook, so the button is
 * <i>here</i>, in the module's own launcher activity.
 *
 * <p><b>The primary button does the whole reset</b> ({@link ResetFlow}), not
 * just a persona rotation. A rotation alone cannot produce a new did: the
 * register server binds the openudid to the {@code install_id} cookie the first
 * time it sees the pair, and TikTok keeps that cookie in stores that are partly
 * below the Java layer. The wipe is what removes it, so the button captures the
 * granted permissions, generates the persona, runs {@code pm clear}, restores
 * the permissions, launches TikTok and reads back the issued did.
 *
 * <p>The secondary button rotates the persona only. It is the path for a device
 * without root, and the UI says plainly that the did will not change.
 */
public class MainActivity extends Activity {

    private static final String TARGET = SandboxIdTT.TARGET;

    private TextView status;
    private TextView personaView;
    private Button newDevice;
    private Button rotateOnly;

    /** Steps arrive from a worker thread; views may only be touched here. */
    private final Handler ui = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        status = new TextView(this);
        personaView = new TextView(this);
        personaView.setTypeface(android.graphics.Typeface.MONOSPACE);
        personaView.setTextSize(12);

        newDevice = new Button(this);
        newDevice.setText("New device persona (one click)");
        newDevice.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                reset();
            }
        });

        rotateOnly = new Button(this);
        rotateOnly.setText("Rotate persona only (no data wipe)");
        rotateOnly.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                rotate();
            }
        });

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);
        root.addView(status);
        root.addView(spacer(8));
        root.addView(newDevice);
        root.addView(rotateOnly);
        root.addView(spacer(16));
        root.addView(personaView);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);

        render();

        // `am start -n id.sandboxid.tt/.MainActivity --ez reset true` runs the
        // same path as the button. This is not a convenience: the flow has to
        // launch TikTok, and Android 10+ aborts a background activity start
        // ("Abort background activity starts from 10306" — observed on this
        // device). A service started over adb has no visible window, so the
        // launch it attempts is dropped. Driving the flow from this activity
        // keeps the module in the foreground for the whole run.
        if (getIntent() != null && getIntent().getBooleanExtra(EXTRA_RESET, false)) {
            getIntent().removeExtra(EXTRA_RESET);
            reset();
        }
    }

    /**
     * Same entry point when the activity is already alive: {@code am start} on
     * a running task delivers the intent here instead of re-creating the
     * activity, so without this the flag would be ignored and a second run
     * would silently do nothing.
     */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (intent != null && intent.getBooleanExtra(EXTRA_RESET, false)) {
            intent.removeExtra(EXTRA_RESET);
            reset();
        }
    }

    /** {@code --ez reset true} on the launch intent. */
    static final String EXTRA_RESET = "reset";

    /** Full reset on a worker thread: it runs root commands and waits ~20 s. */
    private void reset() {
        newDevice.setEnabled(false);
        rotateOnly.setEnabled(false);
        status.setText("Membuat device baru…");

        final StringBuilder trace = new StringBuilder();
        new Thread(new Runnable() {
            public void run() {
                String result = ResetFlow.run(MainActivity.this,
                        new ResetFlow.Step() {
                            public void on(final String line) {
                                ui.post(new Runnable() {
                                    public void run() {
                                        trace.append(line).append('\n');
                                        status.setText(trace.toString());
                                    }
                                });
                            }
                        });
                ui.post(new Runnable() {
                    public void run() {
                        status.setText(result);
                        newDevice.setEnabled(true);
                        rotateOnly.setEnabled(true);
                        render();
                    }
                });
            }
        }, "sbx-reset").start();
    }

    /** Persona rotation without the wipe — the no-root path. */
    private void rotate() {
        Map<String, String> id = PersonaGenerator.generate(this);
        if (id == null || id.isEmpty()) {
            status.setText("Failed: the bundled device pool could not be read. "
                    + "This is a packaging bug — please report it.");
            return;
        }
        IdentityStore.save(this, id);
        render();
        status.setText("Persona baru dibuat, TAPI did tidak akan berganti: data "
                + "TikTok masih menyimpan install_id yang mengikat openudid lama. "
                + "Pakai tombol satu-klik di atas.");
    }

    private void render() {
        SharedPreferences sp = getSharedPreferences(IdentityStore.PREFS, MODE_PRIVATE);
        String blob = sp.getString(IdentityStore.KEY_PERSONA, null);
        if (blob == null) {
            personaView.setText("Belum ada persona. Tekan tombol di atas.");
            status.setText("Modul ini mandiri: tidak butuh modul Zygisk. Aktifkan "
                    + "di LSPosed Manager dengan TikTok di scope, lalu tekan "
                    + "tombol New device.");
            return;
        }
        StringBuilder sb = new StringBuilder();
        sb.append("Dibuat ")
          .append(DateFormat.getDateTimeInstance().format(new Date()))
          .append("\n\n");
        for (String line : blob.split("\n", -1)) {
            if (line.isEmpty()) continue;
            sb.append(line).append('\n');
        }
        personaView.setText(sb.toString());
        if (status.getText().length() == 0) {
            status.setText("Persona siap.");
        }
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
