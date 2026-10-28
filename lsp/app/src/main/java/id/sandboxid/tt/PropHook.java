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
            Map<String, String> id = IdentityStore.load();
            String v = id.get(idk);
            if (v != null && !v.isEmpty()) return v;
            // Derived from the SoC rather than stored. A persona written before
            // these keys existed has no entry for them, so deriving here keeps
            // the fix effective without forcing a re-roll — and it is the same
            // path the native derive_hw_value() takes.
            String d = deriveHwValue(idk, id);
            if (d != null) return d;
        }
        return PropMap.CONSTANT.get(key);
    }

    /**
     * @return the derived value, or null when the key is not derived (or
     *         derives to empty, in which case the prop is hidden instead).
     *         Mirrors {@code derive_hw_value()} in {@code jni/prop_hooks.cpp}.
     */
    private static String deriveHwValue(String idk, Map<String, String> id) {
        String man = orEmpty(id.get("SOC_MANUFACTURER"));
        String mod = orEmpty(id.get("SOC_MODEL"));
        String plat = orEmpty(id.get("BOARD_PLATFORM"));
        switch (idk) {
            case "DALVIK_ARM64_VARIANT":
                return HwNaming.dalvikArm64Variant(man, mod, plat);
            case "DALVIK_ARM_VARIANT":
                return HwNaming.dalvikArmVariant();
            case "GPU_DRIVER": {
                String p = HwNaming.gpuDriverPkg(plat, man, mod);
                return p.isEmpty() ? null : p;
            }
            default:
                return null;
        }
    }

    private static String orEmpty(String s) { return s == null ? "" : s; }

    /**
     * Props the module deliberately reports as absent. Mirrors
     * {@code sbx_prop_hidden()} in {@code jni/module_impl.hpp}, composing the
     * emulator/custom-ROM/vendor-ROM rules with persona-aware OEM rules.
     *
     * <p>The point: a stock Pixel (what the persona claims to be) has none of
     * these, so an in-app read must behave as if the property did not exist —
     * returning the caller's default, not a value that contradicts the persona's
     * Build surface. {@code ro.debuggable}/{@code ro.secure} are deliberately
     * NOT here: the C++ list hides emulator/ROM tells, not root indicators.
     */
    private static boolean isHidden(String key) {
        if (key == null) return false;
        return isEmulatorProp(key) || isCustomRomProp(key) || isVendorRomProp(key)
                || isOemPropHidden(key, IdentityStore.load());
    }

    private static boolean isOemPropHidden(String k, Map<String, String> id) {
        String man = orEmpty(id.get("MANUFACTURER")), brd = orEmpty(id.get("BRAND"));
        boolean xiaomi = man.matches("Xiaomi|Redmi|POCO") || brd.matches("Xiaomi|Redmi|POCO");
        if (!xiaomi && (k.equals("ro.gfx.driver.0") || k.equals("ro.appsflyer.preinstall.path"))) return true;
        if (k.equals("ro.gfx.driver.1"))
            return !HwNaming.isQualcomm(orEmpty(id.get("SOC_MANUFACTURER")), orEmpty(id.get("SOC_MODEL")));
        return false;
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
    //
    // Kept in step with is_vendor_rom_prop() in native/native_read.cpp: the
    // underscore form persist.sys.miui_* is a DIFFERENT namespace from
    // persist.sys.miui.* and slipped past a dot-prefixed test while a target app
    // read it, and the HyperOS scheduler prefixes (turbosched, dexpreload,
    // perf_scenario_recognition, perfdebug, smart_gc) are separate again.
    private static boolean isVendorRomProp(String k) {
        switch (k) {
            case "persist.sys.hardcoder.name":
            case "ro.com.miui.rsa":
            case "ro.boot.cert":
            case "ro.product.cert":
            case "ro.product.odm.cert":
            case "ro.product.vendor.cert":
            case "ro.display.mimotion":
            case "vendor.boostfwk.frameprefetcher":
            case "miui.usb.dialog":
                return true;
            default:
                return k.startsWith("ro.miui.")
                        || k.startsWith("ro.vendor.miui.")
                        || k.startsWith("ro.ril.miui.")
                        || k.startsWith("persist.sys.miui.")
                        || k.startsWith("persist.sys.miui_")
                        || k.startsWith("persist.miui.")
                        || k.startsWith("persist.sys.turbosched.")
                        || k.startsWith("persist.sys.dexpreload.")
                        || k.startsWith("persist.sys.perf_scenario_recognition.")
                        || k.startsWith("persist.sys.perfdebug.")
                        || k.startsWith("persist.sys.smart_gc.")
                        || k.startsWith("ro.vendor.mi_sf.");
        }
    }
}
