// Pins the MIUI/HyperOS hide rule in native_read.cpp against the props a real
// HyperOS device actually exposes.
//
// The rule used to be a list of prefixes, and every prefix was added after a
// measurement showed something leaking through the previous list. That loop
// never converged: after the list "looked complete", 12 props on the reference
// device still reported the ROM. The rule is now a substring test, and this
// suite is what keeps it honest — it feeds the real names in and asserts none
// of them survives.
//
// The names below are copied verbatim from `getprop` on the reference device
// (Redmi alioth / HyperOS 3.0.3). They are data, not a specification: a new
// HyperOS release may add more, which is exactly why the rule is a substring
// test rather than a list.

#include "native_read.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_pass = 0;

static void check(const char* what, bool ok) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("FAIL %s\n", what);
    }
}

int main() {
    // Every MIUI/HyperOS-named prop observed on the reference device. Each one
    // names the ROM to any app that reads it.
    static const char* const kLeaked[] = {
        // Found leaking while the prefix list was still in place.
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
        // Already covered by the old prefix list; kept so a rewrite of the
        // rule cannot regress them.
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

    for (const char* p : kLeaked) {
        check(p, sbxnr::should_hide_prop(p));
    }

    // Case-insensitive: a vendor that ships "MIUI"/"Miui" must not slip past.
    check("upper-case MIUI", sbxnr::should_hide_prop("ro.MIUI.region"));
    check("mixed-case Miui", sbxnr::should_hide_prop("persist.sys.MiuiScout"));

    // The substring rule must NOT swallow props the persona needs. These are
    // the ones a device claiming a Pixel is expected to answer, plus names
    // that merely look similar.
    check("ro.product.model kept",      !sbxnr::should_hide_prop("ro.product.model"));
    check("ro.product.manufacturer kept", !sbxnr::should_hide_prop("ro.product.manufacturer"));
    check("ro.build.fingerprint kept",  !sbxnr::should_hide_prop("ro.build.fingerprint"));
    check("ro.build.version.release kept", !sbxnr::should_hide_prop("ro.build.version.release"));
    check("ro.build.version.sdk kept",  !sbxnr::should_hide_prop("ro.build.version.sdk"));
    check("ro.product.marketname kept", !sbxnr::should_hide_prop("ro.product.marketname"));
    check("ro.product.vendor.marketname kept",
          !sbxnr::should_hide_prop("ro.product.vendor.marketname"));
    check("ro.soc.manufacturer kept",   !sbxnr::should_hide_prop("ro.soc.manufacturer"));
    check("ro.soc.model kept",          !sbxnr::should_hide_prop("ro.soc.model"));
    check("ro.board.platform kept",     !sbxnr::should_hide_prop("ro.board.platform"));
    check("ro.hardware kept",           !sbxnr::should_hide_prop("ro.hardware"));
    check("ro.product.board kept",      !sbxnr::should_hide_prop("ro.product.board"));
    check("ro.serialno kept",           !sbxnr::should_hide_prop("ro.serialno"));
    check("ro.boot.serialno kept",      !sbxnr::should_hide_prop("ro.boot.serialno"));
    check("ro.debuggable kept",         !sbxnr::should_hide_prop("ro.debuggable"));
    check("ro.secure kept",             !sbxnr::should_hide_prop("ro.secure"));
    check("persist.sys.minuit kept",    !sbxnr::should_hide_prop("persist.sys.minuit"));
    check("empty name kept",            !sbxnr::should_hide_prop(""));
    check("null name kept",             !sbxnr::should_hide_prop(nullptr));

    // A name containing "miui" is hidden even when it is not a real prop: the
    // rule is deliberately broad, because the cost of hiding a property that a
    // Pixel would not have anyway is zero, while the cost of leaking one is a
    // coherent-ROM tell.
    check("ro.miuitest.flag hidden",  sbxnr::should_hide_prop("ro.miuitest.flag"));
    check("ro.miui2 hidden",          sbxnr::should_hide_prop("ro.miui2"));

    // Scan boundaries. The rule walks a four-byte window, so a name shorter
    // than the needle must not read past its end, and the match must work at
    // either edge. lsp/.../PropHookTest.java asserts the same set, because a
    // hand-written scan in two languages is exactly where an off-by-one hides.
    check("bare miui hidden",         sbxnr::should_hide_prop("miui"));
    check("bare MIUI hidden",         sbxnr::should_hide_prop("MIUI"));
    check("miui at end hidden",       sbxnr::should_hide_prop("a.miui"));
    check("miui at start hidden",     sbxnr::should_hide_prop("miui.region"));
    check("three letters kept",       !sbxnr::should_hide_prop("miu"));
    check("suffix .miu kept",         !sbxnr::should_hide_prop("ro.product.model.miu"));

    // The other vendor-ROM rules must still work after the rewrite.
    check("ro.gfx.driver.0 hidden",
          sbxnr::should_hide_prop("ro.gfx.driver.0"));
    check("ro.gfx.driver_build_time hidden",
          sbxnr::should_hide_prop("ro.gfx.driver_build_time"));
    check("vendor.camera.aux.packagelist hidden",
          sbxnr::should_hide_prop("vendor.camera.aux.packagelist"));
    check("persist.sys.turbosched.x hidden",
          sbxnr::should_hide_prop("persist.sys.turbosched.x"));
    check("ro.csc.spotify.music.partnerid hidden",
          sbxnr::should_hide_prop("ro.csc.spotify.music.partnerid"));
    check("ro.media.xml_variant.codecs hidden",
          sbxnr::should_hide_prop("ro.media.xml_variant.codecs"));
    check("ro.display.mimotion hidden",
          sbxnr::should_hide_prop("ro.display.mimotion"));
    check("vendor.usb.product_string hidden",
          sbxnr::should_hide_prop("vendor.usb.product_string"));

    // Emulator and custom-ROM rules are untouched by this change.
    check("ro.kernel.qemu hidden", sbxnr::should_hide_prop("ro.kernel.qemu"));
    check("ro.lineage.version hidden",
          sbxnr::should_hide_prop("ro.lineage.version"));

    std::printf("\n%d checks, %d failed\n", g_pass + g_fail, g_fail);
    if (g_fail == 0) std::printf("ALL PASS\n");
    return g_fail == 0 ? 0 : 1;
}
