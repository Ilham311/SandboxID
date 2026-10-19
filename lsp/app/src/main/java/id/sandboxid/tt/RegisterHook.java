package id.sandboxid.tt;

import android.content.SharedPreferences;
import android.util.Log;

import java.lang.reflect.Method;
import java.util.Map;

import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;

/**
 * Forces TikTok's device-register to go to the network and mint a new did.
 *
 * <h3>The three things that must be true</h3>
 *
 * <ol>
 *   <li><b>The body must not carry a previously-registered did.</b>
 *       {@code device_id}/{@code install_id}/{@code bd_did} go to {@code "0"},
 *       which the register server reads as "no device yet".</li>
 *   <li><b>openudid/clientudid/cdid must be values the server has never
 *       seen.</b> A value registered once is bound server-side to that did
 *       forever, and the response echoes it back. Those come from the persona
 *       (see {@link PersonaGenerator}).</li>
 *   <li><b>The replay gate must be open.</b> Verified in
 *       {@code X/088c.LJII(LX/085A;ZZ)V}: when
 *       {@code dr_install_vc == version_code} AND
 *       {@code dr_channel == LX/087j.LJI} AND
 *       {@code dr_aid == String.valueOf(LX/087j.LIZ)},
 *       the method leaves {@code LX/0ARe.LIZLLL} (register_time) alone and the
 *       cached response is replayed — no network call at all. Otherwise it
 *       writes {@code 0} to register_time and registers for real.</li>
 * </ol>
 *
 * <h3>Why the gate is hooked, not just the prefs</h3>
 *
 * <p>Deleting {@code dr_aid}/{@code dr_channel}/{@code dr_install_vc} from the
 * device-parameters store is the documented reset, and this layer does it at
 * install. But the prefs file is a KEVA-backed store with its own in-memory
 * cache in the app process, and the values are re-written by the register
 * itself. Forcing {@code p2 = false} on {@code LJII} is the direct switch: it
 * jumps straight to the {@code iput-wide 0, LX/0ARe;->LIZLLL:J} branch
 * regardless of what the store holds.
 *
 * <h3>Why the wire rewrite is still needed</h3>
 *
 * <p>{@link DeviceParamsHook} fixes the provider, but the body is assembled from
 * many sources ({@code X/08Ak}, {@code X/08BH}, {@code X/08Av}, and the
 * {@code dr_register_param} blob). Rewriting the serialized body at
 * {@code X/087U.post} / {@code X/085T.post} is the last point before the
 * network stack and cannot be undone by a later merge.
 *
 * <h3>What does NOT matter</h3>
 *
 * <ul>
 *   <li>{@code sig_hash} — MD5 of the APK signing certificate, identical on
 *       every TikTok install in the world.</li>
 *   <li>{@code apk_first_install_time} — rewritten for consistency, but
 *       spoofing it alone changed nothing.</li>
 *   <li>The URL query string — register carries only {@code ?req_id=…}.</li>
 * </ul>
 */
final class RegisterHook {

    private static final String TAG = "SandboxID-TT";

    private RegisterHook() {}

    /**
     * Keys whose stored values gate whether register re-runs. Cleared at
     * install so the first launch after a persona rotation always registers.
     */
    static final String[] PURGE_KEYS = {
            "dr_aid", "dr_channel", "dr_install_vc",
    };

    /** Persona keys consumed here. */
    static final String PERSONA_OPENUDID   = "openudid";
    static final String PERSONA_CLIENTUDID = "clientudid";
    static final String PERSONA_CDID       = "cdid";

    /**
     * Shared secret for the did report. The target process can read it; no
     * other app can, because LSPosed's prefs directory is {@code rwx--x--x}
     * for this module's uid.
     */
    static final String PERSONA_NONCE = "report_nonce";

    /** Broadcast the register answer back to the module's own UI. */
    static final String ACTION_ISSUED = "id.sandboxid.tt.DID_ISSUED";
    static final String EXTRA_NONCE   = "nonce";
    static final String EXTRA_DID     = "device_id";
    static final String EXTRA_IID     = "install_id";
    static final String EXTRA_NEW     = "new_user";

    /** Values the broadcast carries; empty when the body had no answer. */
    private static volatile String nonce = "";

    static void apply(ClassLoader cl, final Map<String, String> id) {
        if (id == null) return;

        final String openudid   = HookUtil.get(id, PERSONA_OPENUDID);
        final String clientudid = HookUtil.get(id, PERSONA_CLIENTUDID);
        final String cdid       = HookUtil.get(id, PERSONA_CDID);
        nonce = HookUtil.get(id, PERSONA_NONCE);
        if (nonce == null) nonce = "";

        if (!HookUtil.has(openudid) || !HookUtil.has(clientudid)
                || !HookUtil.has(cdid)) {
            Log.w(TAG, "RegisterHook: persona missing openudid/clientudid/cdid —"
                    + " the register endpoint will not mint a new did."
                    + " Regenerate the persona.");
            return;
        }

        int n = 0;

        // (1) Open the replay gate. X/088c.LJII(LX/085A, boolean, boolean):
        //     arg[1] is the "replay allowed" flag. Forcing it false makes the
        //     method take the register_time = 0 branch unconditionally.
        n += HookUtil.hookByArity(cl, "X.088c", "LJII", 3,
                new XC_MethodHook() {
                    @Override
                    protected void beforeHookedMethod(MethodHookParam p) {
                        if (p.args.length > 1) p.args[1] = Boolean.FALSE;
                    }
                });

        // (2) cdid. Verified chain: X/08Av.LIZ(JSONObject) calls
        //     X/05jB.LIZIZ(LX/087j) and only writes the key when the result is
        //     non-empty. LIZIZ is a static cache over LIZJ(LX/087j), which
        //     reads "cdid" from the device-parameters prefs and falls back to
        //     UUID.randomUUID(). Hooking the two static accessors covers every
        //     caller — including ones that read cdid without going through
        //     X/08Av — and does not depend on the JSONObject shape.
        int cdidHooks = HookUtil.hookByArity(cl, "X.05jB", "LIZIZ", 1,
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam p) {
                        p.setResult(cdid);
                    }
                });
        cdidHooks += HookUtil.hookByArity(cl, "X.05jB", "LIZJ", 1,
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam p) {
                        p.setResult(cdid);
                    }
                });
        // Belt and braces: if X.05jB was renamed, still inject into the body.
        cdidHooks += HookUtil.hookByArity(cl, "X.08Av", "LIZ", 1,
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam p) {
                        try {
                            Object o = p.args[0];
                            if (o instanceof org.json.JSONObject) {
                                ((org.json.JSONObject) o).put("cdid", cdid);
                            }
                        } catch (Throwable t) {
                            Log.w(TAG, "cdid inject failed", t);
                        }
                    }
                });
        n += cdidHooks;

        // (3) The wire. Both concrete implementations of the X.03TU network
        //     interface. Signature post(String, Map, byte[]).
        for (String c : new String[] { "X.087U", "X.085T" }) {
            n += HookUtil.hookByArity(cl, c, "post", 3,
                    new XC_MethodHook() {
                        @Override
                        protected void beforeHookedMethod(MethodHookParam p) {
                            try {
                                Object url = p.args[0];
                                if (url == null) return;
                                String u = String.valueOf(url);
                                if (!u.contains("/device_register/")) return;

                                // Log the header names. The register server has
                                // been observed returning the same did even
                                // after every body identifier was rotated, so
                                // whatever it keys on is outside the body.
                                // Seeing which headers exist is the only way to
                                // tell whether that is a signed fingerprint
                                // (x-argus/x-gorgon, computed in native code and
                                // therefore unreachable from Xposed) or a cookie.
                                Object hdrs = p.args[1];
                                if (hdrs instanceof java.util.Map) {
                                    StringBuilder sb = new StringBuilder();
                                    for (Object e0 : ((java.util.Map) hdrs).entrySet()) {
                                        java.util.Map.Entry e = (java.util.Map.Entry) e0;
                                        if (sb.length() > 0) sb.append(" | ");
                                        sb.append(e.getKey()).append('=').append(e.getValue());
                                    }
                                    Log.i(TAG, "register headers: " + sb);
                                }

                                byte[] body = (byte[]) p.args[2];
                                if (body == null) return;

                                // The body as TikTok built it, before our patch.
                                // Comparing this across personas is how we learn
                                // which fields the app itself never rotates.
                                Log.i(TAG, "register body IN: "
                                        + new String(body, "UTF-8"));

                                byte[] patched = Rewrite.body(body,
                                        openudid, clientudid, cdid);
                                if (patched != null && patched != body) {
                                    p.args[2] = patched;
                                    Log.i(TAG, "register body rewritten: "
                                            + body.length + " -> "
                                            + patched.length);
                                    Log.i(TAG, "register body OUT: "
                                            + new String(patched, "UTF-8"));
                                }
                            } catch (Throwable t) {
                                Log.w(TAG, "register rewrite failed", t);
                            }
                        }

                        /**
                         * The server's answer, which is the only proof of what
                         * did this launch got. {@code new_user:1} here means a
                         * genuinely unseen device; {@code 0} means the openudid
                         * arrived already bound to an install_id — the stores
                         * were not purged. Read this line when a reset looks
                         * like it did nothing.
                         */
                        @Override
                        protected void afterHookedMethod(MethodHookParam p) {
                            try {
                                Object url = p.args[0];
                                if (url == null
                                        || !String.valueOf(url).contains("/device_register/")) {
                                    return;
                                }
                                Object r = p.getResult();
                                String s = r instanceof byte[]
                                        ? new String((byte[]) r, "UTF-8")
                                        : String.valueOf(r);
                                Log.i(TAG, "register response: " + s);
                                report(s);
                            } catch (Throwable ignored) {
                            }
                        }
                    });
        }

        // (4) Purge the replay keys from the device-parameters store. The gate
        //     hook above already forces the network call; this keeps the store
        //     from re-asserting a stale registration on a later launch.
        purge(cl);

        Log.i(TAG, "RegisterHook installed (" + n + " hooks) openudid="
                + openudid);
    }

    /**
     * Sends the did the server issued to the module's own UI.
     *
     * <p><b>Why a broadcast.</b> Without root the module cannot read TikTok's
     * {@code applog_stats.xml}, so the one-click flow has no way to tell the
     * user what did it got. This is the app's own answer, forwarded from inside
     * its process — the same bytes the register call returned, not a local
     * guess.
     *
     * <p><b>Who can see it.</b> The broadcast is package-targeted and carries
     * the persona nonce; {@link MainActivity} drops anything whose nonce does
     * not match the persona it just generated. A third-party app cannot read
     * the nonce, so it cannot forge a report that the UI would accept.
     */
    private static void report(String response) {
        try {
            if (response == null || response.indexOf("\"device_id\"") < 0) return;

            String did = jsonNumber(response, "device_id");
            String iid = jsonNumber(response, "install_id");
            String nw  = jsonNumber(response, "new_user");
            if (did == null) return;

            android.content.Intent i = new android.content.Intent(ACTION_ISSUED);
            i.setPackage("id.sandboxid.tt");
            i.putExtra(EXTRA_NONCE, nonce);
            i.putExtra(EXTRA_DID, did);
            i.putExtra(EXTRA_IID, iid == null ? "" : iid);
            i.putExtra(EXTRA_NEW, nw == null ? "" : nw);
            // A foreground receiver is the only kind the UI registers; this
            // flag is what lets the platform deliver to it.
            i.addFlags(android.content.Intent.FLAG_RECEIVER_FOREGROUND);

            android.content.Context ctx = appContext();
            if (ctx != null) {
                ctx.sendBroadcast(i);
                Log.i(TAG, "reported did " + did + " (new_user=" + nw + ")");
            }
        } catch (Throwable t) {
            Log.w(TAG, "did report failed", t);
        }
    }

    /** {@code "key":123} → {@code "123"}; null when absent. */
    private static String jsonNumber(String json, String key) {
        String k = "\"" + key + "\":";
        int at = json.indexOf(k);
        if (at < 0) return null;
        int i = at + k.length();
        int end = i;
        while (end < json.length()) {
            char c = json.charAt(end);
            if ((c < '0' || c > '9') && c != '-') break;
            end++;
        }
        return end > i ? json.substring(i, end) : null;
    }

    /** The target's own Application, resolved without a stored reference. */
    private static android.content.Context appContext() {
        try {
            Class<?> at = Class.forName("android.app.ActivityThread");
            java.lang.reflect.Method m = at.getDeclaredMethod("currentApplication");
            m.setAccessible(true);
            return (android.content.Context) m.invoke(null);
        } catch (Throwable t) {
            return null;
        }
    }

    /**
     * Deletes {@link #PURGE_KEYS} from every store the device-parameter layer
     * uses.
     *
     * <p>{@code X/089S.LIZ} (verified) resolves the store name as
     * {@code "dp"} or {@code "device_parameters_<aid>"}, optionally suffixed
     * with {@code "_local"}, then {@code ".dat"}. Rather than reconstruct that
     * name, this walks the app's {@code shared_prefs} directory through the
     * target process's own context — which also catches the KEVA-backed
     * adapters that expose a real {@code SharedPreferences} surface.
     */
    private static void purge(ClassLoader cl) {
        try {
            Class<?> at = HookUtil.load(cl, "android.app.ActivityThread");
            if (at == null) return;
            Method current = at.getDeclaredMethod("currentApplication");
            current.setAccessible(true);
            Object app = current.invoke(null);
            if (app == null) return;

            Method getSharedPrefs = android.content.ContextWrapper.class
                    .getMethod("getSharedPreferences", String.class, int.class);
            java.io.File dir = new java.io.File(
                    ((android.content.Context) app).getApplicationInfo().dataDir,
                    "shared_prefs");
            String[] files = dir.list();
            if (files == null) return;

            for (String f : files) {
                if (!f.endsWith(".xml")) continue;
                String name = f.substring(0, f.length() - 4);
                // Only the device-parameter stores carry these keys.
                if (!name.startsWith("dp") && !name.startsWith("device_parameters")) {
                    continue;
                }
                try {
                    Object sp = getSharedPrefs.invoke(app, name,
                            android.content.Context.MODE_PRIVATE);
                    SharedPreferences p = (SharedPreferences) sp;
                    SharedPreferences.Editor e = p.edit();
                    for (String k : PURGE_KEYS) e.remove(k);
                    e.apply();
                    Log.i(TAG, "purged " + PURGE_KEYS.length + " keys from " + name);
                } catch (Throwable ignored) {
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "RegisterHook: purge unavailable", t);
        }
    }
}
