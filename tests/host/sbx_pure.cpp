// Ad-hoc verification harness for the pure functions the L9 read hook relies
// on. Links native_read.cpp directly; no device identity needed.
#include "native_read.hpp"
#include <cassert>
#include <cstdio>
#include <string>

static int fails = 0, checks = 0;

#define CHECK(cond, msg) do { \
    ++checks; \
    if (!(cond)) { ++fails; printf("FAIL: %s\n", msg); } \
    else printf("ok:   %s\n", msg); \
} while (0)

int main() {
    using namespace sbxnr;

    // ---- classify() -------------------------------------------------------
    CHECK(classify("/proc/version") == VERSION, "classify /proc/version");
    CHECK(classify("/proc/meminfo") == MEMINFO, "classify /proc/meminfo");
    CHECK(classify("/proc/cpuinfo") == CPUINFO, "classify /proc/cpuinfo");
    CHECK(classify("/proc/sys/kernel/random/boot_id") == BOOTID, "classify boot_id");
    CHECK(classify("/sys/fs/selinux/enforce") == SELINUX_ENFORCE, "classify selinux enforce");
    CHECK(classify("/sys/class/net/wlan0/address") == MAC, "classify wlan0/address");
    CHECK(classify("/sys/class/net/p2p0/address") == MAC, "classify p2p0/address");
    CHECK(classify("/sys/class/net/dummy0/address") == NONE, "classify dummy0/address (not MAC)");
    CHECK(classify("/data/data/com.app/shared_prefs/applog.xml") == APPLOG_XML, "classify applog.xml");
    CHECK(classify("/data/data/com.app/files/.cdid") == BD_RAW_CDID, "classify .cdid");
    CHECK(classify("/data/data/com.app/files/bd_setting/device_id") == BD_RAW_DID, "classify bd device_id");
    // classify() deliberately matches the full shared_prefs path, not just the
    // filename — a bare suffix match would hijack unrelated files named applog.xml
    CHECK(classify("/sdcard/applog.xml") == NONE, "classify bare applog.xml outside shared_prefs is NONE");
    CHECK(classify("/proc/self/maps") == NONE, "classify /proc/self/maps passthrough");
    CHECK(classify(nullptr) == NONE, "classify nullptr");

    // ---- is_valid_mac -----------------------------------------------------
    CHECK(is_valid_mac("02:1a:2b:3c:4d:5e"), "valid mac");
    CHECK(!is_valid_mac("00:00:00:00:00:00"), "all-zero mac rejected");
    CHECK(!is_valid_mac("zz:1a:2b:3c:4d:5e"), "non-hex mac rejected");
    CHECK(!is_valid_mac("02:1a:2b:3c:4d"), "short mac rejected");
    CHECK(mac_from_seed(12345) == mac_from_seed(12345), "mac deterministic");
    CHECK(mac_from_seed(12345) != mac_from_seed(12346), "mac differs by seed");
    CHECK(mac_from_seed(7)[0] == '0' && mac_from_seed(7)[1] == '2', "mac has locally-administered 02 prefix");

    // ---- applog determinism (the CLI/hook agreement guarantee) ------------
    const std::string fp = "google/raven/raven:13/TQ3A.230901.001/10750268";
    const std::string serial = "ABCDEF12";
    const std::string aid = "deadbeefdeadbeef";
    uint64_t seed = applog_seed(fp, serial, aid, "com.zhiliaoapp.musically");
    uint64_t epoch = 1700000000000ULL;
    ApplogIds a = make_applog_ids(seed, epoch);
    ApplogIds b = make_applog_ids(applog_seed(fp, serial, aid, "com.zhiliaoapp.musically"),
                                  applog_epoch_or_default("1700000000000"));
    CHECK(a.did == b.did && a.iid == b.iid && a.cdid == b.cdid &&
          a.openudid == b.openudid && a.clientudid == b.clientudid && a.ssid == b.ssid,
          "applog ids stable across the two entry points");
    ApplogIds c = make_applog_ids(applog_seed(fp, serial, aid, "other.pkg"), epoch);
    CHECK(a.did != c.did, "applog ids differ per package");
    CHECK(!a.did.empty() && !a.iid.empty() && !a.cdid.empty(), "applog ids non-empty");
    CHECK(applog_epoch_or_default("") == 1700000000000ULL, "epoch default when absent");

    // ---- patch_applog_xml round-trip -------------------------------------
    const std::string real_xml =
        "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n"
        "<map>\n"
        "  <string name=\"device_id\">REALDEVICEID123</string>\n"
        "  <string name=\"install_id\">REALINSTALLID456</string>\n"
        "  <string name=\"ssid\">REALSSID</string>\n"
        "  <string name=\"openudid\">REALOPENUDID</string>\n"
        "  <string name=\"clientudid\">REALCLIENTUDID</string>\n"
        "  <string name=\"cdid\">REALCDID</string>\n"
        "  <string name=\"some_other_key\">keepme</string>\n"
        "</map>\n";
    std::string patched;
    CHECK(patch_applog_xml(real_xml, a, patched), "patch_applog_xml succeeds on a real map");
    CHECK(patched.find("REALDEVICEID123") == std::string::npos, "patch strips real device_id");
    CHECK(patched.find("REALOPENUDID") == std::string::npos, "patch strips real openudid");
    CHECK(patched.find("REALCDID") == std::string::npos, "patch strips real cdid");
    CHECK(patched.find(a.did) != std::string::npos, "patch inserts spoofed did");
    CHECK(patched.find(a.openudid) != std::string::npos, "patch inserts spoofed openudid");
    CHECK(patched.find("keepme") != std::string::npos, "patch keeps unrelated keys");
    CHECK(patched.find("<map") != std::string::npos, "patch keeps xml structure");

    // The synth fallback must contain exactly the 6 spoofed keys
    std::string synth = applog_xml_synth(a);
    for (const char* k : {"device_id", "install_id", "ssid", "openudid", "clientudid", "cdid"}) {
        std::string label = std::string("synth xml has key ") + k;
        CHECK(synth.find(std::string("name=\"") + k + "\"") != std::string::npos,
              label.c_str());
    }

    // patch must refuse something that is not an AppLog map (fail closed)
    std::string junk;
    CHECK(!patch_applog_xml(std::string("not xml at all"), a, junk), "patch rejects non-map input");

    // ---- patch_meminfo ----------------------------------------------------
    const std::string mi =
        "MemTotal:        5918048 kB\n"
        "MemFree:          123456 kB\n"
        "MemAvailable:    4000000 kB\n"
        "HugePages_Total:       0\n";
    std::string mi8 = patch_meminfo(mi, 8);
    CHECK(mi8.find("MemTotal:") == 0, "meminfo patch keeps MemTotal first");
    CHECK(mi8.find("MemFree:") != std::string::npos, "meminfo patch keeps the rest of the file");
    // 8 GB * 1024^3 * 955/1000 kB
    {
        uint64_t expect = 8ULL * 1024 * 1024 * 955 / 1000;
        char buf[64];
        snprintf(buf, sizeof(buf), "MemTotal:       %llu kB", (unsigned long long)expect);
        CHECK(mi8.find(buf) == 0, "meminfo 8GB value exact");
    }
    // unknown model -> the exact kB value must NOT leak; it rounds up to the
    // nearest marketing GB tier (5918048 kB ~ 5.6 GiB -> 6 GB)
    std::string mi_rounded = patch_meminfo(mi, 0);
    CHECK(mi_rounded.find("5918048") == std::string::npos, "meminfo unknown model does not leak the exact real total");
    {
        uint64_t expect = 6ULL * 1024 * 1024 * 955 / 1000;
        char buf[64];
        snprintf(buf, sizeof(buf), "MemTotal:       %llu kB", (unsigned long long)expect);
        CHECK(mi_rounded.find(buf) == 0, "meminfo unknown model rounds 5.6GiB up to the 6GB tier");
    }
    // unparseable real meminfo -> unchanged (do not invent a number)
    CHECK(patch_meminfo(std::string("garbage\nno memtotal here\n"), 0) ==
          std::string("garbage\nno memtotal here\n"),
          "meminfo with no MemTotal line is left alone");

    // ---- patch_cpuinfo ----------------------------------------------------
    const std::string ci_qcom =
        "processor       : 0\n"
        "BogoMIPS        : 38.40\n"
        "Features        : fp asimd\n"
        "Hardware        : Qualcomm Technologies, Inc SM8550\n"
        "Revision        : 0000\n";
    std::string repl;
    int action = cpu_action_for("Qualcomm", "SM8550", repl);
    CHECK(action == CPU_QUALCOMM, "cpu action qualcomm");
    std::string ci_out;
    CHECK(patch_cpuinfo(ci_qcom, action, repl, ci_out), "cpuinfo patched");
    CHECK(ci_out.find("Qualcomm Technologies, Inc SM8550") != std::string::npos, "cpuinfo keeps qualcomm hardware line");
    // a real Tensor cpuinfo (no Hardware line) -> strip action, no patch
    std::string ci_tensor =
        "processor       : 0\n"
        "BogoMIPS        : 38.40\n"
        "Features        : fp asimd evtstrm\n";
    int act2 = cpu_action_for("Google", "GS101", repl);
    CHECK(act2 == CPU_STRIP, "cpu action strip for tensor");
    std::string ci_out2;
    CHECK(!patch_cpuinfo(ci_tensor, act2, repl, ci_out2), "tensor cpuinfo without Hardware line is not modified");

    // ---- synth_proc_version ----------------------------------------------
    std::string pv = synth_proc_version("15", "AP3A.240905.015", "zuma", "abfarm-release-01",
                                        fnv1a("serial"));
    CHECK(pv.find("Linux version 5.15.") == 0, "proc version kernel base for zuma");
    CHECK(pv.find("android15") != std::string::npos, "proc version has android release");
    CHECK(pv.find("clang version") != std::string::npos, "proc version has clang string");

    // ---- uuid / hex helpers ----------------------------------------------
    std::string u = uuid_from_seed(42);
    CHECK(u.size() == 36, "uuid length 36");
    CHECK(u[14] == '4', "uuid is version 4");
    CHECK(u[8] == '-' && u[13] == '-' && u[18] == '-', "uuid dashes placed");
    CHECK(uuid_from_seed(42) == u, "uuid deterministic");
    CHECK(hex_from_seed(1, 32).size() == 64, "hex_from_seed 32 bytes -> 64 chars");

    // ---- snowflake -------------------------------------------------------
    CHECK(snowflake_from_seed(1, 1700000000000ULL) == snowflake_from_seed(1, 1700000000000ULL),
          "snowflake deterministic");
    CHECK(snowflake_from_seed(1, 1700000000000ULL) != snowflake_from_seed(1, 1700000000001ULL),
          "snowflake encodes the epoch");

    // ---- hide lists ------------------------------------------------------
    CHECK(should_hide_prop("ro.kernel.qemu"), "hide ro.kernel.qemu");
    CHECK(should_hide_prop("ro.boot.qemu"), "hide ro.boot.qemu");
    CHECK(should_hide_prop("ro.lineage.build.version"), "hide lineage prop");
    CHECK(!should_hide_prop("ro.build.fingerprint"), "do not hide fingerprint");

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
