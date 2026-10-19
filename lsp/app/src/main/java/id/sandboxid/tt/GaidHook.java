package id.sandboxid.tt;

import android.util.Log;

import java.util.Map;

import de.robv.android.xposed.XC_MethodHook;

/**
 * Serves the persona's Google Advertising ID on the path TikTok reads.
 *
 * <h3>Verified call chain (TikTok 47.1.4)</h3>
 *
 * <pre>
 *   X/02zD.LLLLIILL(Context, LX/02z2) -&gt; AdvertisingIdClient$Info
 *       reflective: "com/google/android/gms/ads/identifier/AdvertisingIdClient"
 *                   "getAdvertisingIdInfo", id 0x2a13
 *       fallback:   AdvertisingIdClient.getAdvertisingIdInfo(ctx)
 *
 *   X/02zD.LLLLIIL(Info, LX/02z2) -&gt; String
 *       reflective: Info.getId(), "()Ljava/lang/String;", id 0x2a05
 *       fallback:   Info.getId()
 * </pre>
 *
 * <p>Two other call sites ({@code X/0MIg}, {@code X/0MIe} in a later dex) also
 * funnel through {@code X/02zD.LLLLIIL}, so hooking that one method covers all
 * three. The {@code Info.getId()} hook is the primary target because it also
 * catches anything that obtains an {@code Info} another way.
 *
 * <h3>Why not patch the GMS store instead</h3>
 *
 * <p>{@code /data/data/com.google.android.gms/shared_prefs/adid_settings.xml}
 * holds the real {@code adid_key}, but it is read by GMS <b>in GMS's own
 * process</b>; TikTok receives only the answer over its binder call. Editing
 * the file would affect every app on the device and still lose to GMS's
 * in-memory cache. The hook has to be in TikTok's process, at the point the
 * value is consumed.
 *
 * <h3>Stability</h3>
 *
 * <p>{@code AdvertisingIdClient$Info} is a GMS/Play-services class and is
 * <b>not</b> obfuscated, so that hook survives TikTok updates. The
 * {@code X/02zD} fallback is obfuscated; it is looked up by name + arity and a
 * miss is logged.
 */
final class GaidHook {

    private static final String TAG = "SandboxID-TT";

    private static final String INFO_CLASS = "com.google.android.gms.ads.identifier.AdvertisingIdClient$Info";

    private static final String FALLBACK_CLASS  = "X.02zD";
    private static final String FALLBACK_METHOD = "LLLLIIL";

    static void apply(ClassLoader cl, final Map<String, String> id) {
        final String gaid = HookUtil.get(id, "GOOGLE_AID");
        if (!HookUtil.has(gaid)) {
            Log.w(TAG, "GaidHook: persona has no GOOGLE_AID — skipping");
            return;
        }

        int n = 0;

        // (1) Info.getId() — one argument (this), returns the GAID string.
        Class<?> info = HookUtil.load(cl, INFO_CLASS);
        if (info != null) {
            for (java.lang.reflect.Method m : info.getDeclaredMethods()) {
                if (!"getId".equals(m.getName())) continue;
                if (m.getParameterTypes().length != 0) continue;
                try {
                    m.setAccessible(true);
                    de.robv.android.xposed.XposedBridge.hookMethod(m,
                            new XC_MethodHook() {
                                @Override
                                protected void afterHookedMethod(MethodHookParam p) {
                                    p.setResult(gaid);
                                }
                            });
                    n++;
                } catch (Throwable t) {
                    Log.w(TAG, "GaidHook: Info.getId hook failed", t);
                }
            }
        } else {
            Log.w(TAG, "GaidHook: " + INFO_CLASS + " not on this classloader");
        }

        // (2) X/02zD.LLLLIIL(Info, token) — the reflective wrapper TikTok uses
        //     when the class above was not directly reachable.
        n += HookUtil.hookByArity(cl, FALLBACK_CLASS, FALLBACK_METHOD, 2,
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam p) {
                        p.setResult(gaid);
                    }
                });

        Log.i(TAG, "GaidHook installed (" + n + " hooks) google_aid=" + gaid);
    }
}
