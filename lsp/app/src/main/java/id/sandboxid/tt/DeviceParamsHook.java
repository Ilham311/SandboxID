package id.sandboxid.tt;

import android.util.Log;

import java.util.Map;

import de.robv.android.xposed.XC_MethodHook;

/**
 * Overrides the identifier provider that the register body is built from.
 *
 * <h3>Why this is a separate layer from {@link RegisterHook}</h3>
 *
 * <p>{@link RegisterHook} rewrites the serialized body on the wire. This layer
 * fixes the <b>source</b>, so every consumer of the provider agrees — the
 * register body, the AppLog header, the KEVA write-back, and anything else that
 * calls {@code DeviceRegisterManager.getOpenUdId()}. Without it, the wire
 * rewrite wins the request but the app immediately overwrites its own store
 * with the server's answer, and the next read disagrees with what was sent.
 *
 * <h3>Verified provider (un-obfuscated, stable across releases)</h3>
 *
 * <pre>
 *   com.bytedance.bdinstall.storage.AbsDeviceParamsProvider
 *     fields  LJ   : String   openudid cache
 *             LJFF : String   clientudid cache
 *             LJI  : volatile String   did cache
 *     methods getDeviceId()  -&gt; LJI, else LIZJ.LIZIZ("","")
 *             getOpenUdid()  -&gt; prefs "openudid", else derive
 *             LIZLLL()       -&gt; prefs "clientudid", else UUID.randomUUID()
 * </pre>
 *
 * <p>{@code getOpenUdid()} has a fallback worth knowing about: when android_id
 * is empty or {@code "9774d56d682e549c"} it generates
 * {@code BigInteger(0x50, SecureRandom).toString(16)}, strips a leading '-',
 * left-pads with {@code 'F'} to 13 characters, then appends {@code "_local"}
 * when {@code LX/087j;->LJJI} is set. Returning the persona directly bypasses
 * that branch entirely — which is the point: the persona value must be the one
 * the server sees, not a value derived from a real device.
 *
 * <p>The cache fields are cleared when the hook installs, so a value read
 * before this layer existed is not replayed for the rest of the process.
 */
final class DeviceParamsHook {

    private static final String TAG = "SandboxID-TT";

    private static final String PROVIDER =
            "com.bytedance.bdinstall.storage.AbsDeviceParamsProvider";

    static void apply(ClassLoader cl, final Map<String, String> id) {
        // The persona stores these under the same keys RegisterHook consumes.
        // Do not spell the names here: an earlier revision read
        // "PERSONA_OPENUDID" (the constant's *name*), found nothing, and
        // silently skipped the whole layer.
        final String openudid   = HookUtil.get(id, RegisterHook.PERSONA_OPENUDID);
        final String clientudid = HookUtil.get(id, RegisterHook.PERSONA_CLIENTUDID);

        if (!HookUtil.has(openudid) || !HookUtil.has(clientudid)) {
            Log.w(TAG, "DeviceParamsHook: persona missing openudid/clientudid");
            return;
        }

        int n = 0;

        // getOpenUdid() -> String. The register body's "openudid".
        // The cache field LJ is written too: some call sites read it directly
        // instead of calling the accessor, and would otherwise keep the value
        // that was derived from the real device.
        n += HookUtil.hookAny(cl, PROVIDER, "getOpenUdid", 0,
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam p) {
                        p.setResult(openudid);
                        HookUtil.setInstanceField(p.thisObject, "LJ", openudid);
                    }
                });

        // LIZLLL() -> String is the clientudid accessor (verified: it reads the
        // "clientudid" pref and falls back to UUID.randomUUID()).
        n += HookUtil.hookAny(cl, PROVIDER, "LIZLLL", 0,
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam p) {
                        p.setResult(clientudid);
                        HookUtil.setInstanceField(p.thisObject, "LJFF", clientudid);
                    }
                });

        // getDeviceId() -> String. "0" tells the register server "no device
        // yet"; the server then mints a fresh did instead of echoing the one
        // this installation was first bound to.
        n += HookUtil.hookAny(cl, PROVIDER, "getDeviceId", 0,
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam p) {
                        p.setResult("0");
                        HookUtil.setInstanceField(p.thisObject, "LJI", "0");
                    }
                });

        Log.i(TAG, "DeviceParamsHook installed (" + n + " hooks)");
    }
}
