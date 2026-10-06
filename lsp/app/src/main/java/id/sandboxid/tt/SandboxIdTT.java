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

        BuildHook.apply(lpparam.classLoader, id);
        PropHook.apply(lpparam.classLoader, id);
        ClockHook.apply(lpparam.classLoader, id);
        TelephonyDrmHook.apply(lpparam.classLoader, id);
        KevaHook.apply(lpparam.classLoader, id);
        RegisterHook.apply(lpparam.classLoader, id);

        Log.i(TAG, "persona applied to " + TARGET);
    }
}
