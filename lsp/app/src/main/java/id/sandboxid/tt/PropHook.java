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
        // Captured once, for the persona-aware hide rule below. Every property
        // read consults that rule, and re-reading the persona from disk each
        // time meant parsing the XML file and hashing it per call.
        persona = id;
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
            Map<String, String> id = persona;
            if (id == null) return null;
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
            // GPU_DRIVER is deliberately NOT derived. ro.gfx.driver.1 names an
            // installed package, and android.os.GraphicsEnvironment.get()
            // checks that with PackageManager: a derived name
            // (com.qualcomm.qti.gpudrivers.<platform>.api30) does not exist on
            // the device, so the framework logs "updatable driver package
            // '<name>' not installed" - a louder tell than the leak. The whole
            // ro.gfx.driver.* family is hidden by isVendorRomProp() instead.
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
                || isOemPropHidden(key);
    }

    /**
     * Persona-aware hides, mirroring {@code sbx_oem_prop_hidden()} in
     * {@code jni/module_impl.hpp}. Kept separate from the pure lists above
     * because these need the identity, which the pure predicates cannot see.
     *
     * <p>{@code ro.gfx.driver.0/.1} are deliberately NOT here — they are hidden
     * unconditionally by {@link #isVendorRomProp}. Hiding {@code .1} only when
     * the persona is non-Qualcomm was the wrong shape twice over: a Qualcomm
     * persona then derived {@code com.qualcomm.qti.gpudrivers.<platform>.api30},
     * a package that is not installed on this device, and
     * {@code GraphicsEnvironment.get()} logs "updatable driver package
     * '<name>' not installed" for it.
     */
    private static boolean isOemPropHidden(String k) {
        // Only ONE name is persona-aware, so check it before touching the
        // persona at all: this runs on every SystemProperties.get() miss.
        if (!"ro.appsflyer.preinstall.path".equals(k)) return false;
        // Read the persona captured at install time, NOT IdentityStore.load().
        // That call constructs a fresh XSharedPreferences, which parses the XML
        // file, hashes it and registers a file watcher — on every property read
        // the app makes. The map is already in hand; re-reading it from disk in
        // a hot path was pure waste and a real cost on a property-heavy process.
        Map<String, String> id = persona;
        if (id == null) return false;
        String man = orEmpty(id.get("MANUFACTURER")), brd = orEmpty(id.get("BRAND"));
        boolean xiaomi = man.matches("Xiaomi|Redmi|POCO") || brd.matches("Xiaomi|Redmi|POCO");
        return !xiaomi;
    }

    /**
     * The persona this process was hooked with, captured once in
     * {@link #apply}. Never null after install; null only if a callback somehow
     * runs before it is assigned, in which case the caller falls through to the
     * real value rather than guessing.
     */
    private static volatile Map<String, String> persona;

    /**
     * Case-insensitive {@code "miui"} substring test, without allocating.
     *
     * <p>{@code k.toLowerCase(Locale.ROOT).contains("miui")} is the obvious
     * form and the one the native side mirrors, but it copies the whole string
     * on every call — and this runs on every {@code SystemProperties.get()}
     * miss, which is the hottest path the module touches. A four-byte window
     * scan over ASCII is enough: property names are ASCII, and a non-ASCII byte
     * can never match one of the four letters anyway.
     */
    private static boolean containsCi(String k, String needle) {
        if (k == null || needle == null) return false;
        final int n = k.length() - needle.length();
        final int nl = needle.length();
        for (int i = 0; i <= n; i++) {
            boolean match = true;
            for (int j = 0; j < nl; j++) {
                if (lower(k.charAt(i + j)) != lower(needle.charAt(j))) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    }

    private static boolean containsMiui(String k) {
        return containsCi(k, "miui");
    }

    private static char lower(char c) {
        return (c >= 'A' && c <= 'Z') ? (char) (c + 32) : c;
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
    //
    // Known gap: the rule is persona-blind, so a Xiaomi/Redmi/POCO persona also
    // loses its MIUI props while denyClasses() keeps miui.os.Build loadable and
    // vendorKeep() keeps the com.miui packages visible. Hiding is right for a
    // Pixel and wrong for a Xiaomi persona whose incremental is
    // V816.0.12.0.UNCMIXM (a real Xiaomi 14 reports ro.miui.ui.version.name=
    // V816); the fix is to derive that prop from the incremental the way
    // deriveHwValue() derives the Dalvik ISA variant. Not done here: it is a
    // behaviour change needing its own device verification, and this batch's job
    // is to make the existing rules actually run.
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
            // Measured on the reference device; each names the real OEM, the
            // real SoC platform, or both.
            case "ro.com.google.clientidbase.pg":        // android-xiaomi-deo2
            case "ro.com.google.lens.oem_image_package": // com.miui.gallery
            case "ro.vendor.extension_library":          // libqti-perfd-client.so
            case "vendor.media.target_variant":          // _kona
            case "vendor.usb.product_string":            // POCO F3
                return true;
            default:
                // The MIUI/HyperOS family, by SUBSTRING rather than prefix.
                //
                // Enumerating prefixes kept leaving holes, and each hole was
                // only found by measuring again: "persist.sys.miui" missed
                // persist.miui.*; adding that missed init.svc.miuibooster and
                // sys.miui_bpf_ready; the ro.* forms missed
                // ro.com.miui.rsa.feature, ro.config.miui_*, and
                // ro.vendor.audio.miui.*. On the reference device 12 props
                // still leaked the ROM after the prefix list was "complete".
                //
                // A stock Pixel has no property whose NAME contains "miui",
                // and no persona value does either (the identity map is
                // derived from Build fields), so the substring test cannot
                // hide anything the persona needs. This is the same reasoning
                // the native side reached; see is_vendor_rom_prop() in
                // native/native_read.cpp.
                if (containsCi(k, "miui") || containsCi(k, "hyperos") || containsCi(k, "turbosched")) {
                    return true;
                }
                return k.startsWith("persist.sys.dexpreload.")
                        || k.startsWith("persist.sys.perf_scenario_recognition.")
                        || k.startsWith("persist.sys.perfdebug.")
                        || k.startsWith("persist.sys.smart_gc.")
                        || k.startsWith("ro.vendor.mi_sf.")
                        || k.startsWith("ro.com.google.clientidbase.")
                        || k.startsWith("ro.vendor.xiaomi.")
                        || k.startsWith("vendor.xiaomi.")
                        || k.startsWith("init.svc.vendor.xiaomi.")
                        // ro.gfx.driver.* — hidden, never derived: see the note
                        // in deriveHwValue(). Two prefixes, not one bare stem:
                        // the real names are ro.gfx.driver.0/.1 (dot) and
                        // ro.gfx.driver_build_time (underscore).
                        || k.startsWith("ro.gfx.driver.")
                        || k.startsWith("ro.gfx.driver_")
                        || k.startsWith("persist.vendor.camera.")
                        || k.startsWith("vendor.camera.")
                        || k.startsWith("ro.csc.spotify.")
                        || k.startsWith("ro.media.xml_variant.");
        }
    }
}
