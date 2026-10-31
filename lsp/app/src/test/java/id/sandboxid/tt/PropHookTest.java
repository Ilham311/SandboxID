package id.sandboxid.tt;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Map;

/**
 * Pins the Java-side prop hide rule against the props a real HyperOS device
 * exposes, and pins that the rule does not re-read the persona from disk.
 *
 * <p>The native equivalent is {@code tests/host/sbx_prop_hide.cpp}; the two must
 * agree, because a persona that is coherent under one variant and not the other
 * is worse than one that is coherent under neither.
 *
 * <p>Runs on a plain JDK: {@code PropHook} only needs {@code android.util.Log}
 * and the Xposed stubs, both of which are stubbed for this build.
 */
public final class PropHookTest {

    private static int pass = 0, fail = 0;

    public static void main(String[] a) throws Exception {
        Method hidden = PropHook.class.getDeclaredMethod("isHidden", String.class);
        hidden.setAccessible(true);

        // ---- every MIUI/HyperOS prop measured on the reference device ------
        String[] leaked = {
            "init.svc.miuibooster",
            "persist.sys.enable_miui_booster",
            "persist.sys.stability.miui_fbo_enable",
            "persist.sys.stability.miui_fbo_start_count",
            "ro.com.miui.rsa.feature",
            "ro.com.miui.rsa.search",
            "ro.config.miui_activity_embedding_enable",
            "ro.config.miui_orientation_projection_enable",
            "ro.vendor.audio.miui.karaoke.show",
            "ro.vendor.audio.miui.karaoke.tencent.show",
            "sys.miui.gki_ndcd",
            "sys.miui_bpf_ready",
            "ro.miui.ui.version.name",
            "ro.miui.ui.version.code",
            "ro.miui.region",
            "ro.miui.notch",
            "ro.miui.mcc",
            "ro.miui.mnc",
            "ro.miui.has_cust_partition",
            "ro.miui.cust_variant",
            "ro.miui.build.region",
            "ro.miui.pai.preinstall.path",
            "ro.miui.allow_app_playbackcapture",
            "ro.miui.block_device_path",
            "ro.miui.customized_clientid",
            "ro.miui.enable_cloud_verify",
            "ro.miui.google.csp",
            "ro.miui.product.home",
            "ro.miui.remove_uri_80_flag",
            "ro.miui.shell_anim_enable_fcb",
            "ro.miui.support_audiorecord_compress",
            "ro.miui.support_force_enable",
            "ro.miui.ui.font.global_theme_apply",
            "ro.miui.ui.font.mi_font_path",
            "ro.miui.vicegwsd",
            "ro.vendor.miui.cust_variant",
            "ro.vendor.miui.mcc",
            "ro.vendor.miui.mnc",
            "ro.vendor.miui.region",
            "persist.miui.density_v2",
            "persist.miui.miperf.enable",
            "persist.miui.wifi.upf.enable",
            "persist.sys.miui.adj_update_foreground_state.enable.delayMs",
            "persist.sys.miui.camera.boost.killAdj_threshold",
            "persist.sys.miui.camera.boost.opt",
            "persist.sys.miui.camera.boost.task",
            "persist.sys.miui.camera.protect_prev_ext",
            "persist.sys.miui.sno",
            "persist.sys.miui.sys_monitor",
            "persist.sys.miui_animator_sched.big_prime_cores",
            "persist.sys.miui_animator_sched.bigcores",
            "persist.sys.miui_gnss_pc",
            "persist.sys.miui_scout_binder_full_kill_process",
            "persist.sys.miui_scout_enable",
            "persist.sys.miui_sptm.enable",
            "persist.sys.miui_sptm.enable_pl_type",
            "persist.sys.miuibooster.name",
            "persist.sys.hardcoder.name",
            "miui.usb.dialog",
            "ro.com.miui.rsa",
        };
        for (String p : leaked) check("hidden: " + p, true, hidden.invoke(null, p));

        // Case-insensitive.
        check("hidden: ro.MIUI.region", true, hidden.invoke(null, "ro.MIUI.region"));
        check("hidden: persist.sys.MiuiScout", true,
                hidden.invoke(null, "persist.sys.MiuiScout"));

        // Substring edge cases: the scan must not run off the end, and a name
        // that merely starts with the letters must not match.
        check("hidden: miui", true, hidden.invoke(null, "miui"));
        check("hidden: MIUI", true, hidden.invoke(null, "MIUI"));
        check("hidden: a.miui", true, hidden.invoke(null, "a.miui"));
        check("hidden: miuix (contains miui)", true, hidden.invoke(null, "miuix"));
        check("kept: miu (too short to contain miui)", false,
                hidden.invoke(null, "miu"));
        check("kept: ro.product.model.miu", false,
                hidden.invoke(null, "ro.product.model.miu"));

        // ---- props the persona needs must survive -------------------------
        String[] keep = {
            "ro.product.model", "ro.product.manufacturer", "ro.build.fingerprint",
            "ro.build.version.release", "ro.build.version.sdk",
            "ro.product.marketname", "ro.product.vendor.marketname",
            "ro.soc.manufacturer", "ro.soc.model", "ro.board.platform",
            "ro.hardware", "ro.product.board", "ro.serialno", "ro.boot.serialno",
            "ro.debuggable", "ro.secure", "persist.sys.minuit",
        };
        for (String p : keep) check("kept: " + p, false, hidden.invoke(null, p));
        check("kept: empty", false, hidden.invoke(null, ""));
        check("kept: null", false, hidden.invoke(null, (Object) null));

        // ---- the other vendor-ROM rules still hold ------------------------
        check("hidden: ro.gfx.driver.0", true,
                hidden.invoke(null, "ro.gfx.driver.0"));
        check("hidden: ro.gfx.driver_build_time", true,
                hidden.invoke(null, "ro.gfx.driver_build_time"));
        check("hidden: vendor.camera.aux.packagelist", true,
                hidden.invoke(null, "vendor.camera.aux.packagelist"));
        check("hidden: persist.sys.turbosched.x", true,
                hidden.invoke(null, "persist.sys.turbosched.x"));
        check("hidden: bpf.hyperos_progs_loaded", true,
                hidden.invoke(null, "bpf.hyperos_progs_loaded"));
        check("hidden: persist.sys.turbosched.coreApp.enable", true,
                hidden.invoke(null, "persist.sys.turbosched.coreApp.enable"));
        check("hidden: ro.com.google.clientidbase.ms", true,
                hidden.invoke(null, "ro.com.google.clientidbase.ms"));
        check("hidden: ro.vendor.xiaomi.bl.poll", true,
                hidden.invoke(null, "ro.vendor.xiaomi.bl.poll"));
        check("hidden: init.svc.vendor.xiaomi.hardware.fingerprintextension-service", true,
                hidden.invoke(null, "init.svc.vendor.xiaomi.hardware.fingerprintextension-service"));
        check("hidden: ro.csc.spotify.music.partnerid", true,
                hidden.invoke(null, "ro.csc.spotify.music.partnerid"));
        check("hidden: ro.media.xml_variant.codecs", true,
                hidden.invoke(null, "ro.media.xml_variant.codecs"));
        check("hidden: ro.display.mimotion", true,
                hidden.invoke(null, "ro.display.mimotion"));
        check("hidden: vendor.usb.product_string", true,
                hidden.invoke(null, "vendor.usb.product_string"));
        check("hidden: ro.kernel.qemu", true, hidden.invoke(null, "ro.kernel.qemu"));
        check("hidden: ro.lineage.version", true,
                hidden.invoke(null, "ro.lineage.version"));

        // ---- the persona-aware appsflyer rule -----------------------------
        // Non-Xiaomi persona: the HyperOS preinstall path must be hidden.
        setPersona("MANUFACTURER", "Google", "BRAND", "google");
        check("appsflyer hidden for Pixel persona", true,
                hidden.invoke(null, "ro.appsflyer.preinstall.path"));
        // Xiaomi persona: it is that vendor's own prop, so it stays.
        setPersona("MANUFACTURER", "Xiaomi", "BRAND", "Redmi");
        check("appsflyer kept for Xiaomi persona", false,
                hidden.invoke(null, "ro.appsflyer.preinstall.path"));
        // No persona at all: fail safe, do not hide (the app sees the real
        // value, which is the pre-fix behaviour rather than a wrong answer).
        setPersona(null, null, null, null);
        check("appsflyer kept when no persona", false,
                hidden.invoke(null, "ro.appsflyer.preinstall.path"));

        // ---- the hot path must not re-read the persona from disk ----------
        // IdentityStore.load() constructs XSharedPreferences, which parses the
        // XML and hashes it. Calling it per SystemProperties read was a real
        // cost. The rule now reads the map captured at install time; this
        // asserts the captured map is what is consulted, by changing it and
        // observing the answer change without touching disk.
        setPersona("MANUFACTURER", "OPPO", "BRAND", "oppo");
        check("persona swap takes effect immediately (1)", true,
                hidden.invoke(null, "ro.appsflyer.preinstall.path"));
        setPersona("MANUFACTURER", "POCO", "BRAND", "POCO");
        check("persona swap takes effect immediately (2)", false,
                hidden.invoke(null, "ro.appsflyer.preinstall.path"));

        System.out.println();
        System.out.println(pass + " checks, " + fail + " failed");
        System.out.println(fail == 0 ? "ALL PASS" : "FAILED");
        if (fail != 0) System.exit(1);
    }

    /** Sets PropHook.persona directly — the field the install path fills. */
    private static void setPersona(String k1, String v1, String k2, String v2)
            throws Exception {
        Map<String, String> m = new HashMap<>();
        if (k1 != null) m.put(k1, v1);
        if (k2 != null) m.put(k2, v2);
        Field f = PropHook.class.getDeclaredField("persona");
        f.setAccessible(true);
        f.set(null, m.isEmpty() ? null : m);
    }

    private static void check(String what, boolean want, Object got) {
        boolean g = Boolean.TRUE.equals(got);
        if (g == want) { pass++; }
        else {
            fail++;
            System.out.println("FAIL " + what + ": want " + want + " got " + g);
        }
    }
}
