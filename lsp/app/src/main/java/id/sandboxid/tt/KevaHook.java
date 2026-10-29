package id.sandboxid.tt;

import android.util.Log;

import java.util.Map;

import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;

/**
 * Overrides TikTok's own key-value store on the read path, so a persona's ids
 * win over whatever the app has persisted.
 *
 * <p><b>Replaces the previous {@code AppLogHook}.</b> That hook intercepted
 * {@code android.app.SharedPreferencesImpl.getString} for {@code applog.xml} and
 * {@code snssdk_openudid.xml} — files TikTok 47.1.4 does not create. It hooked a
 * store that was never read, which is the definition of a no-op, so it has been
 * removed. This hook targets the store that is actually read.
 *
 * <h3>Why KEVA, not SharedPreferences</h3>
 *
 * <p>ByteDance replaced SharedPreferences with an in-house log-structured store
 * ("KEVA") for the identity values that matter. The reader the app actually
 * calls is {@code com.bytedance.keva.adapter.KevaSpFastAdapter.getString}; the
 * values live under {@code files/keva/repo/<name>/<name>.blk}. A
 * SharedPreferencesImpl hook sees none of that traffic.
 *
 * <h3>The two layers that must both be hooked</h3>
 *
 * <p>TikTok reads these keys through three adapters with different obfuscated
 * names — {@code X.02tJ} (musically) and {@code X.02t3} (trill) in addition to
 * the un-obfuscated KevaSpFastAdapter — and which one a code path uses varies by
 * key AND by build. All are hooked because hooking only some leaves those reads
 * returning the stored value. This was observed directly: patching one adapter
 * left identity fields leaking.
 *
 * <p><b>Log-structured caveat.</b> A KEVA {@code .blk} is an append log; old
 * values remain in the file as garbage after being updated. Do not read these
 * files with {@code strings | head} to verify a value — it returns a stale
 * entry. Use {@code tail}, or read via this hook instead.
 *
 * <h3>Why the write-back does not defeat this</h3>
 *
 * <p>The register response is merged back into the store by
 * {@code X.088K.LIZIZ} (see {@link RegisterHook}). That merge happens, and then
 * a subsequent read returns the merged value. {@link RegisterHook} forces the
 * merged did itself to "0", and this hook supplies the persona's value, so a
 * read after the write-back still yields the persona — not the did the app was
 * first registered with.
 *
 * <h3>Stability</h3>
 *
 * <p>{@code KevaSpFastAdapter} is un-obfuscated and has been stable here.
 * {@code X.02tJ} is obfuscated and will be renamed on a TikTok update; the hook
 * is looked up by name and a failure is logged, so a rename costs this layer
 * rather than the process.
 */
final class KevaHook {

    private static final String TAG = "SandboxID-TT";

    private KevaHook() {}

    /**
     * KEVA read adapters, all of which are live code paths.
     *
     * <p>Three names, and the third is not redundant. The obfuscated sibling is
     * per-build, and the two builds' classes are NOT the same thing under a
     * different name — in trill 47.0.3, {@code X.02tJ} is a five-method enum
     * with no {@code getString} at all. The real twin is {@code X.02t3}:
     *
     * <pre>
     *   musically 47.1.4 : LX/02tJ;  18 methods (full SharedPreferences)
     *   trill     47.0.3 : LX/02t3;  18 methods (full SharedPreferences)
     * </pre>
     *
     * <p>Verified by comparing the two method sets: identical except for three
     * parameter types renamed with everything else. Hooking only the first two
     * left the trill build reading the persisted openudid/clientudid/cdid
     * straight from its cache.
     */
    private static final String[] ADAPTERS = {
            "com.bytedance.keva.adapter.KevaSpFastAdapter",
            "X.02tJ",
            "X.02t3",
    };

    static void apply(ClassLoader cl, final Map<String, String> id) {
        if (id == null || id.isEmpty()) return;

        for (String c : ADAPTERS) {
            try {
                XposedHelpers.findAndHookMethod(c, cl, "getString",
                        String.class, String.class, new XC_MethodHook() {
                            @Override
                            protected void afterHookedMethod(MethodHookParam p) {
                                try {
                                    String k = (String) p.args[0];
                                    if (k == null) return;
                                    String v = id.get(k);
                                    if (v != null) p.setResult(v);
                                } catch (Throwable ignored) {
                                }
                            }
                        });
                XposedBridge.log(TAG + " KevaHook: " + c + " hooked");
            } catch (Throwable t) {
                // X.02tJ renames per release; a miss degrades this layer.
                Log.w(TAG, "KevaHook: " + c + " unavailable", t);
            }
        }
    }
}
