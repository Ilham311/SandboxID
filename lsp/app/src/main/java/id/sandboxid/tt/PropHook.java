package id.sandboxid.tt;

import android.util.Log;

import java.util.Map;

import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedHelpers;

/**
 * Hooks {@code android.os.SystemProperties} on the Java surface.
 *
 * <p>Port of the native {@code install_prop_hook} + {@code spoof_prop_value()}
 * ({@code jni/prop_hooks.cpp}). The C++ side replaces the JNI native methods
 * {@code native_get}/{@code native_get_int/long/boolean}; under Xposed the
 * idiomatic surface is the public Java wrappers themselves, which reach the same
 * values through the same call chain.
 *
 * <p>Decision order is identical to {@code hook_prop_get}:
 * <ol>
 *   <li>identity-mapped prop &rarr; persona value (via {@link PropMap#IDENTITY});</li>
 *   <li>constant prop &rarr; its literal (via {@link PropMap#CONSTANT});</li>
 *   <li>otherwise pass through to the real property service.</li>
 * </ol>
 *
 * <p><b>Known gap, stated up front.</b> The Zygisk module additionally
 * PLT-hooks the <i>native</i> {@code __system_property_find/get/
 * read_callback} symbols, so a probe compiled into the app's own .so sees the
 * persona even when it never touches Java. Xposed has no GOT-redirect API, so
 * this variant covers the Java lens only. A native probe reading
 * {@code __system_property_get} directly sees the real value.
 */
final class PropHook {

    private static final String TAG = "SandboxID-TT";

    private PropHook() {}

    static void apply(ClassLoader cl, final Map<String, String> id) {
        // String get(String, String)
        XposedHelpers.findAndHookMethod("android.os.SystemProperties", cl,
                "get", String.class, String.class, new XC_MethodHook() {
                    @Override
                    protected void beforeHookedMethod(MethodHookParam p) {
                        String key = (String) p.args[0];
                        Object def = p.args[1];
                        String v = resolve(key);
                        if (v != null) {
                            p.setResult(v);
                        } else if (isHidden(key)) {
                            // Report absent: return the caller's default, as the
                            // native hook does for sbx_prop_hidden().
                            p.setResult(def);
                        }
                    }
                });

        // int getInt(String, int)
        XposedHelpers.findAndHookMethod("android.os.SystemProperties", cl,
                "getInt", String.class, int.class, new XC_MethodHook() {
                    @Override
                    protected void beforeHookedMethod(MethodHookParam p) {
                        String v = resolve((String) p.args[0]);
                        if (v != null) {
                            try { p.setResult(Integer.parseInt(v.trim())); }
                            catch (NumberFormatException ignored) {
                                // Out of jint range: the native hook deliberately
                                // falls through rather than clamp. Do the same.
                            }
                        }
                    }
                });

        // long getLong(String, long)
        XposedHelpers.findAndHookMethod("android.os.SystemProperties", cl,
                "getLong", String.class, long.class, new XC_MethodHook() {
                    @Override
                    protected void beforeHookedMethod(MethodHookParam p) {
                        String v = resolve((String) p.args[0]);
                        if (v != null) {
                            try { p.setResult(Long.parseLong(v.trim())); }
                            catch (NumberFormatException ignored) { }
                        }
                    }
                });

        // boolean getBoolean(String, boolean)
        XposedHelpers.findAndHookMethod("android.os.SystemProperties", cl,
                "getBoolean", String.class, boolean.class, new XC_MethodHook() {
                    @Override
                    protected void beforeHookedMethod(MethodHookParam p) {
                        String v = resolve((String) p.args[0]);
                        if (v != null) {
                            // Same truthiness as SystemProperties.native_get_boolean
                            p.setResult(!"0".equals(v.trim())
                                    && !"false".equalsIgnoreCase(v.trim()));
                        }
                    }
                });

        Log.d(TAG, "SystemProperties hooks installed");
    }

    /** @return spoof value, or null to pass through. */
    private static String resolve(String key) {
        if (key == null) return null;
        String idk = PropMap.IDENTITY.get(key);
        if (idk != null) {
            String v = IdentityStore.load().get(idk);
            if (v != null && !v.isEmpty()) return v;
        }
        return PropMap.CONSTANT.get(key);
    }

    /**
     * Props the module deliberately reports as absent. Direct port of
     * {@code should_hide_prop()} in {@code native/native_read.cpp}, which
     * composes {@code is_emulator_prop} + {@code is_custom_rom_prop} +
     * {@code is_vendor_rom_prop}.
     *
     * <p>The point: a stock Pixel (what the persona claims to be) has none of
     * these, so an in-app read must behave as if the property did not exist —
     * returning the caller's default, not a value that contradicts the persona's
     * Build surface. {@code ro.debuggable}/{@code ro.secure} are deliberately
     * NOT here: the C++ list hides emulator/ROM tells, not root indicators.
     */
    private static boolean isHidden(String key) {
        if (key == null) return false;
        return isEmulatorProp(key) || isCustomRomProp(key) || isVendorRomProp(key);
    }

    private static boolean isEmulatorProp(String k) {
        switch (k) {
            case "ro.kernel.qemu":
            case "ro.kernel.qemu.gles":
            case "ro.boot.qemu":
            case "ro.boot.qemu.gltransport":
            case "ro.hardware.virtual_device":
            case "qemu.hw.mainkeys":
            case "init.svc.qemud":
            case "init.svc.qemu-props":
            case "init.svc.goldfish-logcat":
            case "init.svc.goldfish-setup":
            case "init.svc.ranchu-net":
                return true;
            default:
                return k.startsWith("qemu.")
                        || k.startsWith("ro.kernel.qemu.")
                        || k.startsWith("ro.boot.qemu.");
        }
    }

    private static boolean isCustomRomProp(String k) {
        switch (k) {
            case "ro.modversion":
            case "ro.cm.version":
            case "ro.cm.build.date":
                return true;
            default:
                return k.startsWith("ro.lineage.")
                        || k.startsWith("lineage.")
                        || k.startsWith("ro.cm.")
                        || k.startsWith("persist.sys.lineage.");
        }
    }

    // MIUI / HyperOS: a stock Pixel has none of these. Observed on a HyperOS
    // device: ro.miui.ui.version.name=V816, persist.sys.hardcoder.name=
    // miui_booster — unambiguous OEM tells the persona's Build surface
    // contradicts. Same treatment as the custom-ROM props above.
    private static boolean isVendorRomProp(String k) {
        switch (k) {
            case "persist.sys.hardcoder.name":
            case "ro.com.miui.rsa":
                return true;
            default:
                return k.startsWith("ro.miui.")
                        || k.startsWith("ro.vendor.miui.")
                        || k.startsWith("ro.ril.miui.")
                        || k.startsWith("persist.sys.miui.");
        }
    }
}
