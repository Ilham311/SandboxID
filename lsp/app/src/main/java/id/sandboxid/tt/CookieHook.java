package id.sandboxid.tt;

import android.util.Log;

import java.util.Map;

import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;

/**
 * Strips the {@code install_id}/{@code device_id} cookies from every cookie
 * string TikTok reads.
 *
 * <h3>Why this layer is required</h3>
 *
 * <p>Verified on-device against the live register endpoint (curl, no app
 * involved), varying one field at a time:
 *
 * <pre>
 *   body openudid = fresh, no cookie          -> NEW did, new_user:1
 *   body openudid = fresh, Cookie: install_id -> OLD did, new_user:0
 *   body openudid = old,   no cookie          -> OLD did (already bound)
 * </pre>
 *
 * <p>So the register server binds {@code openudid} to the {@code install_id}
 * cookie the <b>first</b> time it sees that pair. TikTok always sends its
 * stored {@code install_id} cookie, which means a brand-new persona openudid is
 * bound to the old did on its very first request — and every later request
 * echoes that did. The body rewrite in {@link RegisterHook} cannot prevent
 * this: the cookie is attached by TTNet below the Java hook layer, where the
 * {@code post(url, headers, body)} argument map no longer contains it.
 *
 * <h3>What this hooks</h3>
 *
 * <p>Both cookie surfaces the app can read from Java. The value is filtered,
 * not blanked: every other cookie in the string is left alone, because
 * {@code odin_tt}/{@code ttreq}/{@code store-idc} carry region and anti-bot
 * state the request needs.
 *
 * <p>If TikTok ever moves to a native-only cookie store this layer goes quiet
 * and the symptom returns — a new persona reusing the old did. That failure is
 * visible in the logs as "no install_id cookie to strip", so it is diagnosable
 * rather than silent.
 */
final class CookieHook {

    private static final String TAG = "SandboxID-TT";

    private CookieHook() {}

    /** Cookie names that bind a device to a previously registered did. */
    private static final String[] DROP = {
            "install_id", "device_id", "bd_did", "sessionid",
    };

    static void apply(ClassLoader cl, final Map<String, String> id) {
        int n = 0;

        // android.webkit.CookieManager — used by the urlconnection path
        // (X/03FY.LIZ builds a Cookie header from this on some endpoints).
        n += HookUtil.hookAny(cl, "android.webkit.CookieManager", "getCookie", 1,
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam p) {
                        Object r = p.getResult();
                        if (r instanceof String) {
                            String s = strip((String) r);
                            if (!s.equals(r)) {
                                p.setResult(s);
                                Log.i(TAG, "stripped identity cookie (webkit)");
                            }
                        }
                    }
                });

        // java.net.CookieStore.get(URI) — the other Java-side surface.
        for (String c : new String[] {
                "java.net.CookieStore", "java.net.InMemoryCookieStore" }) {
            n += HookUtil.hookAny(cl, c, "get", 1, new XC_MethodHook() {
                @Override
                protected void afterHookedMethod(MethodHookParam p) {
                    Object r = p.getResult();
                    if (r instanceof java.util.List) {
                        java.util.List<?> in = (java.util.List<?>) r;
                        java.util.List<Object> out = new java.util.ArrayList<>();
                        for (Object o : in) {
                            if (o instanceof java.net.HttpCookie) {
                                String name = ((java.net.HttpCookie) o).getName();
                                if (name != null && isDrop(name)) continue;
                            }
                            out.add(o);
                        }
                        if (out.size() != in.size()) p.setResult(out);
                    }
                }
            });
        }

        Log.i(TAG, "CookieHook installed (" + n + " hooks)");
    }

    private static boolean isDrop(String name) {
        for (String d : DROP) {
            if (d.equalsIgnoreCase(name)) return true;
        }
        return false;
    }

    /** Removes {@code name=value} pairs from a {@code a=1; b=2} cookie string. */
    static String strip(String cookies) {
        if (cookies == null || cookies.isEmpty()) return cookies;
        String[] parts = cookies.split(";");
        StringBuilder sb = new StringBuilder(cookies.length());
        boolean changed = false;
        for (String raw : parts) {
            String p = raw.trim();
            if (p.isEmpty()) continue;
            int eq = p.indexOf('=');
            String name = eq < 0 ? p : p.substring(0, eq).trim();
            if (isDrop(name)) {
                changed = true;
                continue;
            }
            if (sb.length() > 0) sb.append("; ");
            sb.append(p);
        }
        return changed ? sb.toString() : cookies;
    }
}
