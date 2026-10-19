package id.sandboxid.tt;

import android.os.Build;
import android.util.Log;

import java.lang.reflect.Field;
import java.util.Map;

import de.robv.android.xposed.XposedHelpers;

/**
 * Sets the {@code android.os.Build} surface to the persona.
 *
 * <p>This is a 1:1 port of {@code install_build_hook()} in
 * {@code jni/module_hooks.cpp}: the same field list, the same fallback rule
 * (skip empty values), and the same {@code Build.TIME} seconds&rarr;millis
 * scaling. Each native {@code SetStaticObjectField} maps onto
 * {@link XposedHelpers#setStaticObjectField}.
 *
 * <p><b>Timing caveat (verified against the C++ side).</b> The Zygisk module
 * patches these fields in {@code postAppSpecialize}, before any app class
 * initialises. {@code handleLoadPackage} runs <i>after</i> class load, so a
 * {@code static final} constant or a cached {@code Build.MODEL} captured at
 * class-init has already taken the real value. This layer covers the common
 * runtime reads; a class-init-time consumer is a known gap in the Xposed
 * variant and must be checked per target.
 */
final class BuildHook {

    private static final String TAG = "SandboxID-TT";

    /** {Build field, identity key} — identical order to the C++ table. */
    private static final String[][] STRING_FIELDS = {
            {"MODEL", "MODEL"},
            {"BRAND", "BRAND"},
            {"MANUFACTURER", "MANUFACTURER"},
            {"DEVICE", "DEVICE"},
            {"PRODUCT", "PRODUCT"},
            {"BOARD", "BOARD"},
            {"HARDWARE", "HARDWARE"},
            {"DISPLAY", "DISPLAY"},
            {"ID", "ID"},
            {"FINGERPRINT", "FINGERPRINT"},
            {"HOST", "HOST"},
            {"TAGS", "TAGS"},
            {"TYPE", "TYPE"},
            {"USER", "USER"},
            {"SOC_MANUFACTURER", "SOC_MANUFACTURER"},
            {"SOC_MODEL", "SOC_MODEL"},
    };

    private BuildHook() {}

    static void apply(ClassLoader cl, Map<String, String> id) {
        int set = 0;
        for (String[] f : STRING_FIELDS) {
            String v = id.get(f[1]);
            if (v == null || v.isEmpty()) continue;
            try {
                XposedHelpers.setStaticObjectField(Build.class, f[0], v);
                set++;
            } catch (Throwable t) {
                Log.w(TAG, "Build." + f[0] + " not set", t);
            }
        }

        // Build.VERSION
        String rel = id.get("RELEASE");
        if (rel != null && !rel.isEmpty()) {
            trySet(Build.VERSION.class, "RELEASE", rel);
        }
        String sdk = id.get("SDK_INT");
        if (sdk != null && !sdk.isEmpty()) {
            try {
                XposedHelpers.setStaticIntField(Build.VERSION.class, "SDK_INT",
                        Integer.parseInt(sdk.trim()));
            } catch (Throwable ignored) {
                // Out-of-int range would be a broken persona; keep the real value
                // rather than crash, mirroring sbx_parse_longlong's fall-through.
            }
        }
        String inc = id.get("INCREMENTAL");
        if (inc != null && !inc.isEmpty()) trySet(Build.VERSION.class, "INCREMENTAL", inc);
        String sp = id.get("SECURITY_PATCH");
        if (sp != null && !sp.isEmpty()) trySet(Build.VERSION.class, "SECURITY_PATCH", sp);

        // Build.TIME is seconds in the identity, milliseconds in the field.
        String bt = id.get("BUILD_TIME_UTC");
        if (bt != null && !bt.isEmpty()) {
            try {
                long t = Long.parseLong(bt.trim());
                if (t > 0) {
                    if (t < 4102444800L) t *= 1000L;   // seconds -> milliseconds
                    XposedHelpers.setStaticLongField(Build.class, "TIME", t);
                    set++;
                }
            } catch (Throwable ignored) {
            }
        }

        Log.d(TAG, "Build hook: " + set + " field(s) spoofed");
    }

    private static void trySet(Class<?> clz, String field, String v) {
        try {
            XposedHelpers.setStaticObjectField(clz, field, v);
        } catch (Throwable t) {
            Log.w(TAG, clz.getSimpleName() + "." + field + " not set", t);
        }
    }
}
