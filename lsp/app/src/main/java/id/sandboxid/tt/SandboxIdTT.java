package id.sandboxid.tt;

import android.util.Log;

import java.util.Map;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

/**
 * Entry point for the LSPosed variant. Loaded into every app in the declared
 * scope; the scope itself is restricted to TikTok by the manifest's
 * {@code xposedscope} resource, so this gate is defence in depth rather than
 * the only barrier.
 *
 * <p>The Zygisk module unloads itself for non-targets at
 * {@code module.cpp} preAppSpecialize; the equivalent here is simply returning.
 *
 * <p><b>Standalone.</b> Everything this variant needs — the persona generator,
 * the storage the hooks read, the AppLog synthesis — is inside this APK. It does
 * not read anything from {@code /data/adb} and does not require the Zygisk
 * module to be installed.
 */
public class SandboxIdTT implements IXposedHookLoadPackage {

    static final String TAG = "SandboxID-TT";

    static final String TARGET = "com.zhiliaoapp.musically";

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam lpparam) {
        if (lpparam == null || !TARGET.equals(lpparam.packageName)) return;
        final ClassLoader cl = lpparam.classLoader;

        Map<String, String> id = IdentityStore.load();
        if (id == null || id.isEmpty()) {
            // The user has not generated a persona yet. Nothing is spoofed,
            // which is the correct failure mode: do not half-apply.
            Log.i(TAG, "no persona yet — open the SandboxID TT app and press "
                    + "Generate, then restart TikTok. Nothing is being spoofed.");
            return;
        }

        Log.i(TAG, "applying persona to " + lpparam.packageName
                + " (" + id.size() + " keys)");

        // The register server binds an openudid to the install_id cookie it is
        // first seen with, permanently. Verified on-device against the live
        // endpoint (curl, no app): a fresh openudid returns a fresh did with
        // new_user:1, but the *same* openudid with an install_id cookie, or a
        // second time at all, returns the did it was first bound to.
        //
        // So each launch derives a new openudid. This keeps the persona's
        // device identity (model/build/etc.) while giving the register call an
        // identifier the server has not seen. It is not sufficient on its own:
        // the app also has to be starting from a state where it has no stored
        // install_id to send, which is what KevaHook/CookieHook address and
        // what `pm clear` guarantees (see the README's reset procedure).
        String freshOpenudid = PersonaGenerator.freshOpenudid(id);
        if (freshOpenudid != null) {
            id.put(RegisterHook.PERSONA_OPENUDID, freshOpenudid);
            Log.i(TAG, "openudid rotated for this launch: " + freshOpenudid);
        }

        // Purge before anything else runs. handleLoadPackage is called before
        // the target's Application.onCreate, so deleting the identity stores
        // here means KEVA and TTNet open a store that does not yet exist. This
        // is the no-root equivalent of the `pm clear` in ResetFlow: a fresh
        // openudid is necessary but not sufficient, because the register
        // server binds it to whatever install_id cookie arrives with it, and
        // that cookie is rebuilt from these stores.
        layer("purge", new Runnable() {
            public void run() { PurgeHook.apply(lpparam.appInfo, id); }
        });

        // Each layer is isolated: a NoClassDefFoundError while enumerating one
        // obfuscated class (TikTok ships optional integrations whose types are
        // not always present) must not take the remaining layers down. Before
        // this, one missing Apple SDK class aborted the whole method and the
        // identity hooks were never installed.
        layer("build", new Runnable() {
            public void run() { BuildHook.apply(cl, id); }
        });
        layer("props", new Runnable() {
            public void run() { PropHook.apply(cl, id); }
        });
        layer("clock", new Runnable() {
            public void run() { ClockHook.apply(cl, id); }
        });
        layer("telephony", new Runnable() {
            public void run() { TelephonyDrmHook.apply(cl, id); }
        });

        // Identity layer — the paths TikTok 47.1.4 actually reads. These must
        // come before KevaHook/RegisterHook: the register body is assembled
        // from what they return.
        layer("android_id", new Runnable() {
            public void run() { AndroidIdHook.apply(cl, id); }
        });
        layer("gaid", new Runnable() {
            public void run() { GaidHook.apply(cl, id); }
        });
        layer("device_params", new Runnable() {
            public void run() { DeviceParamsHook.apply(cl, id); }
        });

        layer("keva", new Runnable() {
            public void run() { KevaHook.apply(cl, id); }
        });
        layer("cookie", new Runnable() {
            public void run() { CookieHook.apply(cl, id); }
        });
        layer("register", new Runnable() {
            public void run() { RegisterHook.apply(cl, id); }
        });

        // Lets this process be ended on request so the next start is cold.
        // Without it a second reset while TikTok stays alive rotates the
        // persona and registers nothing: hooks run once per process.
        layer("self_restart", new Runnable() {
            public void run() { SelfRestart.apply(cl, id); }
        });

        Log.i(TAG, "persona applied to " + TARGET);
    }

    /** Runs one hook layer, logging and swallowing anything it throws. */

    /** Runs one hook layer, logging and swallowing anything it throws. */
    private static void layer(String name, Runnable r) {
        try {
            r.run();
        } catch (Throwable t) {
            Log.e(TAG, "layer '" + name + "' failed: " + t, t);
        }
    }
}
