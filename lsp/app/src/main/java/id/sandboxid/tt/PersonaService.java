package id.sandboxid.tt;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

import java.util.Map;

/**
 * Headless persona generation and reset, for testing and for automation.
 *
 * <p><b>Why this exists.</b> {@link MainActivity} is the normal path, but the
 * module has to be testable without a human tapping a button — on a locked
 * device, in a script, or over adb. Starting a service works while the
 * keyguard is up, so this is the one entry point that always runs.
 *
 * <p><b>Two actions.</b> {@link #ACTION_GENERATE} rotates the persona only.
 * {@link #ACTION_RESET} runs the whole one-click path from {@link ResetFlow}:
 * capture permissions, rotate, {@code pm clear}, restore, launch, read the
 * issued did. The second one is what actually changes the did; the first is
 * kept because it is the only thing that works without root.
 *
 * <p><b>Who may call it.</b> The manifest guards the service with a
 * {@code signature}-level permission, so a normal app cannot start it; root
 * (uid 0) bypasses permission checks, which is what makes {@code su -c} work.
 * The uid check cannot be done here: by the time {@link #onStartCommand} runs,
 * the binder caller identity has been cleared and {@link Binder#getCallingUid()}
 * reports this process's own uid, not the caller's.
 *
 * <p>Usage:
 * <pre>
 *   su -c 'am startservice -n id.sandboxid.tt/.PersonaService \
 *       -a id.sandboxid.tt.GENERATE'
 *   su -c 'am startservice -n id.sandboxid.tt/.PersonaService \
 *       -a id.sandboxid.tt.RESET'
 * </pre>
 */
public class PersonaService extends Service {

    private static final String TAG = "SandboxID-TT";

    static final String ACTION_GENERATE = "id.sandboxid.tt.GENERATE";
    static final String ACTION_RESET    = "id.sandboxid.tt.RESET";

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, final int startId) {
        String action = intent == null ? null : intent.getAction();

        if (ACTION_RESET.equals(action)) {
            // pm clear + launch take tens of seconds; do not block the service
            // thread, which the system will kill for an ANR.
            new Thread(new Runnable() {
                public void run() {
                    String report = ResetFlow.run(PersonaService.this,
                            new ResetFlow.Step() {
                                public void on(String line) {
                                    Log.i(TAG, "reset: " + line);
                                }
                            });
                    Log.i(TAG, "reset done:\n" + report);
                    stopSelf(startId);
                }
            }, "sbx-reset").start();
            return START_NOT_STICKY;
        }

        if (!ACTION_GENERATE.equals(action)) {
            stopSelf(startId);
            return START_NOT_STICKY;
        }

        Map<String, String> id = PersonaGenerator.generate(this);
        if (id == null || id.isEmpty()) {
            Log.e(TAG, "PersonaService: device pool unreadable — packaging bug");
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        IdentityStore.save(this, id);
        Log.i(TAG, "PersonaService: generated persona, "
                + id.size() + " keys, android_id=" + id.get("ANDROID_ID")
                + " openudid=" + id.get(RegisterHook.PERSONA_OPENUDID));
        stopSelf(startId);
        return START_NOT_STICKY;
    }
}
