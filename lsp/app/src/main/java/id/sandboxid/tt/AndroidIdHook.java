package id.sandboxid.tt;

import android.util.Log;

import java.util.Map;

import de.robv.android.xposed.XC_MethodHook;

/**
 * Serves the persona's {@code android_id} on the path TikTok actually reads.
 *
 * <h3>Why the old hook was not enough</h3>
 *
 * <p>{@code Settings.Secure.getString(cr, "android_id")} <b>is</b> called by
 * TikTok 47.1.4 — verified at {@code X/02zD.LLFZ}, which invokes it reflectively
 * with method id {@code 0x2805}. But that call sits at the <i>bottom</i> of a
 * chain that caches its answer, so patching it alone changes nothing after the
 * first read:
 *
 * <pre>
 *   X/08Dg.LIZ(ContentResolver)                  &lt;- entry point
 *     if (TextUtils.isEmpty(X/02yq.LIZ))
 *         X/02yq.LIZ = X/02zD.LLFZ(cr, "android_id", LX/02z2)
 *     return X/02yq.LIZ                          &lt;- static cache, process lifetime
 *
 *   X/02zD.LLFZ(cr, key, token)
 *     reflective: android/provider/Settings$Secure.getString(cr, key)
 *     fallback:   Settings$Secure.getString(cr, key)
 * </pre>
 *
 * <p>{@code X/02yq} is not a reader: it holds {@code LIZ()Z} (a kids-mode gate
 * over {@code kids_mode_overseas}) and the {@code LIZ:String} cache. The cache
 * is the reason a hook on {@code getString} alone looked dead.
 *
 * <h3>What this layer does</h3>
 *
 * <ol>
 *   <li>Clears {@code X/02yq.LIZ} once at install, so a value read before the
 *       hook landed is not replayed.</li>
 *   <li>Hooks {@code X/08Dg.LIZ(ContentResolver)} — one argument, returns the
 *       persona. This closes every caller at once.</li>
 *   <li>Hooks {@code X/02zD.LLFZ(...)} — the fallback if (2) is renamed. Only
 *       {@code "android_id"} is answered; every other key passes through, so
 *       this does not turn into a blanket Settings reader.</li>
 * </ol>
 *
 * <p>The third layer, {@code Settings$Secure.getString} itself, is already
 * hooked in {@link TelephonyDrmHook} and stays there — it also covers direct
 * reads that never go through the two classes above.
 *
 * <h3>Stability</h3>
 *
 * <p>Both classes are obfuscated and rename per release. Lookups are by name +
 * arity through {@link HookUtil}, and a miss is logged rather than thrown: a
 * rename degrades to "android_id not intercepted", not a crash.
 */
final class AndroidIdHook {

    private static final String TAG = "SandboxID-TT";

    /** Class that memoises android_id for the process lifetime. */
    private static final String CACHE_HOLDER = "X.02yq";
    private static final String CACHE_FIELD  = "LIZ";

    /** The entry point every TikTok caller goes through. */
    private static final String ENTRY_CLASS  = "X.08Dg";
    private static final String ENTRY_METHOD = "LIZ";

    /** The reflective Settings reader underneath it. */
    private static final String READER_CLASS  = "X.02zD";
    private static final String READER_METHOD = "LLFZ";

    static void apply(ClassLoader cl, final Map<String, String> id) {
        final String androidId = HookUtil.get(id, "ANDROID_ID");
        if (!HookUtil.has(androidId)) {
            Log.w(TAG, "AndroidIdHook: persona has no ANDROID_ID — skipping");
            return;
        }

        // (1) Drop whatever the app memoised before this hook existed.
        HookUtil.clearStatic(cl, CACHE_HOLDER, CACHE_FIELD);

        // (2) The entry point: X/08Dg.LIZ(ContentResolver) -> String.
        int n = HookUtil.hookByArity(cl, ENTRY_CLASS, ENTRY_METHOD, 1,
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam p) {
                        p.setResult(androidId);
                    }
                });

        // (3) The reader underneath: X/02zD.LLFZ(cr, key, token) -> String.
        //     Only "android_id" is answered; any other key is left alone so
        //     this hook cannot leak into unrelated Settings reads.
        n += HookUtil.hookByArity(cl, READER_CLASS, READER_METHOD, 3,
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam p) {
                        try {
                            Object k = p.args[1];
                            if (k != null && "android_id".equals(String.valueOf(k))) {
                                p.setResult(androidId);
                            }
                        } catch (Throwable ignored) {
                        }
                    }
                });

        Log.i(TAG, "AndroidIdHook installed (" + n + " hooks) android_id="
                + androidId);
    }
}
