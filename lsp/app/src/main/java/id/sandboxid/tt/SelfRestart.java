package id.sandboxid.tt;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.util.Log;

import java.util.Map;

/**
 * Lets the module's own UI end the target process so the next launch is cold.
 *
 * <h3>Why the target has to end itself</h3>
 *
 * <p>Hooks install once per process. A running TikTok keeps the persona it was
 * started with, and {@link PurgeHook} only runs at start — so a second reset
 * while the app stays alive rotates the persona and then registers nothing.
 * Verified: the pid was unchanged across three consecutive resets, and only the
 * first (which followed a root {@code force-stop}) minted a did.
 *
 * <p>Without root there is no way to stop another app's process from outside.
 * {@code ActivityManager.killBackgroundProcesses} is refused for anything
 * perceptible, and TikTok holds foreground services, so it is a no-op here
 * (measured: pid survived). {@code am force-stop} needs root.
 *
 * <p>What is always possible is the process ending <b>itself</b>. This receiver
 * lives in the target's process (installed from
 * {@code handleLoadPackage}); on request it purges the identity stores and then
 * calls {@code Process.killProcess}, after which the UI launches TikTok again
 * and the fresh process picks up the new persona with no stored ids.
 *
 * <h3>Why this cannot be abused</h3>
 *
 * <p>The action is public, so any app can send it — which would be a way to kill
 * TikTok. The receiver therefore requires the persona nonce, which lives in
 * LSPosed's prefs directory ({@code rwx--x--x}, this module's uid only). An app
 * that cannot read the persona cannot produce a request this accepts.
 */
final class SelfRestart {

    private static final String TAG = "SandboxID-TT";

    static final String ACTION = "id.sandboxid.tt.RESTART";
    static final String EXTRA_NONCE = "nonce";

    private SelfRestart() {}

    /**
     * Installs the receiver in the target process.
     *
     * <p><b>Why it hooks instead of registering immediately.</b>
     * {@code handleLoadPackage} runs before the target's Application exists, so
     * {@code ActivityThread.currentApplication()} is still null there and there
     * is no Context to register against. {@code callApplicationOnCreate} is the
     * first point where one exists, and it is still before any of TikTok's own
     * startup work.
     *
     * @param cl    the target's ClassLoader
     * @param nonce the persona's report nonce; requests must match it
     */
    static void apply(ClassLoader cl, final Map<String, String> id) {
        final String nonce = id == null ? null : id.get(IdentityStore.KEY_RESTART);
        if (nonce == null || nonce.isEmpty()) {
            Log.w(TAG, "SelfRestart: no restart key, receiver not installed");
            return;
        }
        try {
            de.robv.android.xposed.XposedHelpers.findAndHookMethod(
                    "android.app.Instrumentation", cl,
                    "callApplicationOnCreate", android.app.Application.class,
                    new de.robv.android.xposed.XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam p) {
                            try {
                                android.app.Application app =
                                        (android.app.Application) p.args[0];
                                install(app, nonce);
                            } catch (Throwable t) {
                                Log.w(TAG, "SelfRestart: install failed", t);
                            }
                        }
                    });
        } catch (Throwable t) {
            Log.w(TAG, "SelfRestart: cannot hook Instrumentation", t);
        }
    }

    private static void install(Context ctx, final String nonce) {
        BroadcastReceiver rx = new BroadcastReceiver() {
            @Override
            public void onReceive(Context c, Intent i) {
                try {
                    if (i == null) return;
                    if (!nonce.equals(i.getStringExtra(EXTRA_NONCE))) return;
                    Log.i(TAG, "SelfRestart: requested — purging and exiting");
                    // Forced: the marker may already match this generation,
                    // but the point of the request is that the next process
                    // must start with nothing stored.
                    PurgeHook.applyForced(c.getApplicationInfo(), IdentityStore.load());
                    android.os.Process.killProcess(android.os.Process.myPid());
                } catch (Throwable t) {
                    Log.w(TAG, "SelfRestart failed", t);
                }
            }
        };
        IntentFilter f = new IntentFilter(ACTION);
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            // Context.RECEIVER_EXPORTED, spelled out: the module's UI is a
            // different uid and the android.jar used here predates the
            // constant. The nonce is what makes exporting safe.
            ctx.registerReceiver(rx, f, 0x2);
        } else {
            ctx.registerReceiver(rx, f);
        }
        Log.i(TAG, "SelfRestart: receiver installed");
    }
}
