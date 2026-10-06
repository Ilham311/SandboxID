package id.sandboxid.tt;

import android.util.Log;

import java.util.Map;

import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;

/**
 * Makes TikTok's device-register endpoint mint a <i>new</i> device id, instead
 * of re-binding to the one this installation was first recognised under.
 *
 * <p><b>This is the load-bearing hook of the LSPosed variant.</b> Everything
 * else (Build, props, telephony, DRM) changes what the device <i>looks like</i>;
 * this hook is what makes the ByteDance register server actually issue a fresh
 * did for it. It was found by on-device experiment, not by reading code:
 *
 * <p>The register flow is {@code X/0ARf.LIZIZ()} ("Register#doRegister") ->
 * {@code X/0iul.LJ(...)} -> {@code X/03TU.post(...)}. The concrete
 * implementations of the {@code X.03TU} interface are {@code X.087U} and
 * {@code X.085T}, both with the signature {@code post(String, Map, byte[])}.
 * That last one is the wire: hooking it shows the request the server actually
 * receives, and rewriting its body is what changes the server's answer.
 *
 * <h3>What actually produces a new did</h3>
 *
 * <p>Verified on-device, TikTok 47.1.4, by observing the register request and
 * response end to end. Three consecutive launches, three distinct dids, and the
 * previously-bound did appeared in <b>zero</b> of them:
 *
 * <ol>
 *   <li><b>The body must carry no previously-registered did.</b> The app sends
 *       {@code device_id}/{@code install_id}/{@code bd_did} as their last-known
 *       values. They are rewritten to {@code "0"}, which the register server
 *       reads as "no device yet".</li>
 *   <li><b>openudid, clientudid and cdid must be values the server has never
 *       seen.</b> A value that has been registered once is bound server-side to
 *       that did forever, and the register response will simply echo it back.
 *       The persona generator must therefore derive these from the persona seed
 *       — never reuse a constant — and rotating the persona must change them.</li>
 *   <li><b>The register re-run must be allowed to happen.</b> Register is gated
 *       on {@code dr_aid}/{@code dr_channel}/{@code dr_install_vc} in the KEVA
 *       repo {@code ug_install_settings_pref}; when all three match the current
 *       values the app replays a cached response instead of hitting the network.
 *       A user doing "generate persona, then kill and reopen TikTok" restarts
 *       the process, which is sufficient — but a persona rotation that does not
 *       change those three values will replay the cache and silently no-op.
 *       See {@link #PURGE_KEYS}.</li>
 * </ol>
 *
 * <h3>What does NOT matter</h3>
 *
 * <p>Ruled out by experiment, do not re-investigate:
 * <ul>
 *   <li>{@code sig_hash} — MD5 of the APK signing certificate. Constant for every
 *       TikTok install in the world, so it cannot identify a device.</li>
 *   <li>{@code apk_first_install_time} — the real first-install timestamp. It is
 *       rewritten here for consistency, but spoofing it alone changed nothing.</li>
 *   <li>GAID — null on the test device.</li>
 *   <li>The URL query string — register carries only {@code ?req_id=…}; the
 *       identifying params appear on other endpoints ({@code /dsign/}), not here.</li>
 *   <li>A "burned id" that no longer helps — the ids simply have to be fresh.</li>
 * </ul>
 *
 * <h3>The write-back, and why we do not block it</h3>
 *
 * <p>The register response is written back through
 * {@code AbsDeviceParamsProvider.LIZJ(String)} -> {@code X.088K.LIZIZ}, which
 * merges the server's did into the device-id store. Blocking that hook was
 * useful as a diagnostic, but in production we <b>want</b> the write-back: the
 * freshly minted did is the one the app should now identify as. The
 * read-path KEVA overrides in {@link KevaHook} keep the persona stable on
 * <i>subsequent</i> reads; this hook only wins the register round-trip itself.
 *
 * <h3>Stability note</h3>
 *
 * <p>{@code com.bytedance.bdinstall.storage.AbsDeviceParamsProvider} and
 * {@code com.bytedance.keva.adapter.KevaSpFastAdapter} are un-obfuscated and
 * have been stable across the versions checked here. The single-letter classes
 * ({@code X.087U}, {@code X.085T}, {@code X.0iul}, {@code X.088K}) are renamed
 * per release; every hook on them is looked up by name at load time and a
 * failure is logged rather than swallowed, so a rename degrades into "register
 * not intercepted" instead of a crash.
 */
final class RegisterHook {

    private static final String TAG = "SandboxID-TT";

    private RegisterHook() {}

    /**
     * KEVA keys whose stored values gate whether register re-runs. If the app's
     * current aid/channel/version_code all match these, register is skipped and
     * a cached response is replayed — which looks exactly like the server
     * recognising the device, but is purely local. Clearing them is the
     * "1-klik new device" reset for this layer.
     */
    static final String[] PURGE_KEYS = {
            "dr_aid", "dr_channel", "dr_install_vc",
    };

    /** Body keys that must read as "no device registered yet". */
    private static final String[] ZERO_KEYS = {
            "device_id", "install_id", "bd_did", "last_update_sender_did",
    };

    /** Body keys whose values must never have been seen by the register server. */
    static final String PERSONA_OPENUDID   = "openudid";
    static final String PERSONA_CLIENTUDID = "clientudid";
    static final String PERSONA_CDID       = "cdid";
    static final String PERSONA_INSTALL_INFO = "install_info";

    static void apply(ClassLoader cl, Map<String, String> id) {
        if (id == null) return;

        String openudid   = id.get(PERSONA_OPENUDID);
        String clientudid = id.get(PERSONA_CLIENTUDID);
        String cdid       = id.get(PERSONA_CDID);

        // The persona must supply these. Without fresh values the register
        // server echoes back whatever did the openudid was first bound to, so
        // this layer cannot do its job. Fail loudly rather than half-apply.
        if (openudid == null || clientudid == null || cdid == null) {
            Log.w(TAG, "RegisterHook: persona is missing openudid/clientudid/"
                    + "cdid — the register endpoint will not mint a new did."
                    + " Regenerate the persona.");
            return;
        }

        int n = 0;

        // (1) The device-identity provider. Un-obfuscated, and the source the
        // register header reads directly. getDeviceId() must return "0" or the
        // request body advertises the did we are trying to replace.
        try {
            XposedHelpers.findAndHookMethod(
                    "com.bytedance.bdinstall.storage.AbsDeviceParamsProvider", cl,
                    "getDeviceId",
                    new XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam p) {
                            p.setResult("0");
                        }
                    });
            XposedHelpers.findAndHookMethod(
                    "com.bytedance.bdinstall.storage.AbsDeviceParamsProvider", cl,
                    "getOpenUdid",
                    new XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam p) {
                            p.setResult(openudid);
                        }
                    });
            n += 2;
        } catch (Throwable t) {
            // Not fatal: the body rewrite below still wins on the register path.
            Log.w(TAG, "RegisterHook: AbsDeviceParamsProvider unavailable", t);
        }

        // (2) The merge that defeats a read-path override. When a did is written
        // back, X.088K.LIZIZ merges it into the store and the identity provider
        // caches the merged value, so subsequent getDeviceId() calls return it.
        // Force any long numeric key back to "0" so the stored did can never
        // outrank the persona. This hook is load-bearing: without it, the
        // write-back silently undoes (1) for the rest of the process lifetime.
        try {
            XposedHelpers.findAndHookMethod("X.088K", cl, "LIZIZ",
                    String.class, String.class, new XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam p) {
                            try {
                                String k = (String) p.args[0];
                                if (k != null && k.length() >= 10
                                        && isAllDigits(k)) {
                                    p.setResult("0");
                                }
                            } catch (Throwable ignored) {
                            }
                        }
                    });
            n++;
        } catch (Throwable t) {
            Log.w(TAG, "RegisterHook: X.088K.LIZIZ unavailable", t);
        }

        // (3) The wire. Rewrite the register body at the last moment, so every
        // code path that assembles it is covered regardless of which loader
        // populated it. The byte[] form is the serialized JSON, so we patch the
        // string and re-encode.
        for (String c : new String[] { "X.087U", "X.085T" }) {
            try {
                XposedHelpers.findAndHookMethod(c, cl, "post",
                        String.class, java.util.Map.class, byte[].class,
                        new XC_MethodHook() {
                            @Override
                            protected void beforeHookedMethod(MethodHookParam p) {
                                try {
                                    Object url = p.args[0];
                                    if (url == null) return;
                                    String u = String.valueOf(url);
                                    if (!u.contains("/device_register/")) return;

                                    byte[] body = (byte[]) p.args[2];
                                    if (body == null) return;

                                    byte[] patched = Rewrite.body(body,
                                            openudid, clientudid, cdid);
                                    if (patched != null && patched != body) {
                                        p.args[2] = patched;
                                        Log.i(TAG, "register body rewritten: "
                                                + body.length + " -> "
                                                + patched.length);
                                    }
                                } catch (Throwable t) {
                                    Log.w(TAG, "register rewrite failed", t);
                                }
                            }
                        });
                n++;
            } catch (Throwable t) {
                Log.w(TAG, "RegisterHook: " + c + ".post unavailable", t);
            }
        }

        Log.i(TAG, "RegisterHook installed (" + n + " hooks) openudid="
                + openudid);
    }

    private static boolean isAllDigits(String s) {
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c < '0' || c > '9') return false;
        }
        return true;
    }
}
