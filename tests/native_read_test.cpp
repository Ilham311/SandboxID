
#include "../jni/sbx_native_read.hpp"
#include "../jni/sbx_mountinfo.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <algorithm>

using namespace sbxnr;

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond, msg) do {                                            \
    ++g_checks;                                                          \
    if (!(cond)) { ++g_fails; std::printf("FAIL: %s  (%s:%d)\n",         \
                                          (msg), __FILE__, __LINE__); }  \
} while (0)

static bool is_hexlc(const std::string& s) {
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

static void test_uuid() {
    uint64_t seed = fnv1a("google/husky/husky:14/AP1A.240505.004/11583682:user/release-keys");
    std::string u1 = uuid_from_seed(seed);
    std::string u2 = uuid_from_seed(seed);
    CHECK(u1 == u2, "uuid deterministic for same seed");
    CHECK(u1.size() == 36, "uuid length 36");
    CHECK(u1[8] == '-' && u1[13] == '-' && u1[18] == '-' && u1[23] == '-',
          "uuid dash positions");
    CHECK(u1[14] == '4', "uuid version nibble == 4");
    char var = u1[19];
    CHECK(var == '8' || var == '9' || var == 'a' || var == 'b',
          "uuid variant nibble in {8,9,a,b}");

    std::string stripped;
    for (char c : u1) if (c != '-') stripped.push_back(c);
    CHECK(stripped.size() == 32 && is_hexlc(stripped), "uuid body is 32 lowercase hex");

    uint64_t seed2 = fnv1a("google/husky/husky:14/AP1A.240505.004/11583683:user/release-keys");
    CHECK(uuid_from_seed(seed2) != u1, "uuid differs for different seed");
}

static void test_mac() {
    uint64_t seed = fnv1a("husky-serial-ABC123");
    std::string m1 = mac_from_seed(seed);
    std::string m2 = mac_from_seed(seed);
    CHECK(m1 == m2, "mac deterministic");
    CHECK(m1.size() == 17, "mac length 17");
    CHECK(m1.substr(0, 3) == "02:", "mac first octet is locally-administered 02");
    CHECK(is_valid_mac(m1), "generated mac is valid");
    CHECK(mac_from_seed(fnv1a("other")) != m1, "mac differs for different seed");

    CHECK(!is_valid_mac(""), "empty mac invalid");
    CHECK(!is_valid_mac("02:00:00:00:00"), "short mac invalid");
    CHECK(!is_valid_mac("00:00:00:00:00:00"), "all-zero mac invalid");
    CHECK(!is_valid_mac("02-00-11-22-33-44"), "wrong-separator mac invalid");
    CHECK(!is_valid_mac("0g:00:11:22:33:44"), "non-hex mac invalid");
    CHECK(is_valid_mac("aa:bb:cc:dd:ee:ff"), "lowercase mac valid");
    CHECK(is_valid_mac("AA:BB:CC:DD:EE:FF"), "uppercase mac valid");
}

static void test_mac_bytes_and_iface() {

    uint8_t b[6] = {0};
    CHECK(mac_str_to_bytes("02:11:22:33:44:55", b), "decode returns true for valid mac");
    CHECK(b[0] == 0x02 && b[1] == 0x11 && b[2] == 0x22 &&
          b[3] == 0x33 && b[4] == 0x44 && b[5] == 0x55, "decode bytes correct");

    uint8_t bu[6] = {0};
    CHECK(mac_str_to_bytes("AA:BB:CC:DD:EE:FF", bu), "decode uppercase mac");
    CHECK(bu[0] == 0xAA && bu[3] == 0xDD && bu[5] == 0xFF, "uppercase decode correct");

    uint8_t bs[6] = {0};
    CHECK(mac_str_to_bytes(mac_from_seed(fnv1a("husky")), bs), "seed mac decodes");
    CHECK(bs[0] == 0x02, "seed mac locally-administered byte preserved");

    uint8_t bad[6] = {0};
    CHECK(!mac_str_to_bytes("", bad), "empty mac not decoded");
    CHECK(!mac_str_to_bytes("00:00:00:00:00:00", bad), "all-zero mac not decoded");
    CHECK(!mac_str_to_bytes("0g:00:11:22:33:44", bad), "non-hex mac not decoded");

    CHECK(is_wifi_iface("wlan0"), "wlan0 is wifi");
    CHECK(is_wifi_iface("wlan1"), "wlan1 is wifi");
    CHECK(is_wifi_iface("p2p0"), "p2p0 is wifi");
    CHECK(is_wifi_iface("p2p-wlan0-0"), "p2p-dev is wifi");
    CHECK(!is_wifi_iface("eth0"), "eth0 not wifi");
    CHECK(!is_wifi_iface("rmnet0"), "rmnet0 not wifi (cellular has no persistent MAC)");
    CHECK(!is_wifi_iface("lo"), "lo not wifi");
    CHECK(!is_wifi_iface(nullptr), "null iface not wifi");
}

static void test_proc_version() {

    std::string v = synth_proc_version("14", "11583682", "zuma", "abfarm42", 0x1234abcd);
    CHECK(v.rfind("Linux version ", 0) == 0, "version starts 'Linux version '");
    CHECK(v.find("-android14-") != std::string::npos, "android14 token present");
    CHECK(v.find("5.15.") != std::string::npos, "zuma -> kernel 5.15");
    CHECK(v.find("clang version 17.0.4") != std::string::npos, "A14 -> clang 17.0.4");
    CHECK(v.find("#1 SMP PREEMPT") != std::string::npos, "SMP PREEMPT present");
    CHECK(v.find("abfarm42") != std::string::npos, "build host present");

    CHECK(synth_proc_version("14", "11583682", "zuma", "abfarm42", 0x1234abcd) == v,
          "proc/version deterministic");

    std::string g6 = synth_proc_version("14", "123456", "gs101", "", 42);
    CHECK(g6.find("5.10.") != std::string::npos, "gs101 -> kernel 5.10 even on A14");

    std::string a15 = synth_proc_version("15", "999999", "", "", 7);
    CHECK(a15.find("6.1.") != std::string::npos, "A15 no-platform -> kernel 6.1");
    CHECK(a15.find("clang version 18.0.1") != std::string::npos, "A15 -> clang 18.0.1");
    CHECK(a15.find("-android15-") != std::string::npos, "android15 token");

    std::string zp = synth_proc_version("15", "888888", "zumapro", "", 5);
    CHECK(zp.find("6.1.") != std::string::npos, "zumapro -> kernel 6.1");

    std::string er = synth_proc_version("", "", "", "", 1);
    CHECK(er.rfind("Linux version ", 0) == 0, "empty inputs still Linux version");
    CHECK(er.find("-android14-") != std::string::npos, "empty release -> android14 fallback");
    CHECK(er.find("-ab") != std::string::npos, "ab build token present even w/ empty incremental");
}

static void test_meminfo() {
    std::string real =
        "MemTotal:        7654321 kB\n"
        "MemFree:         1234567 kB\n"
        "MemAvailable:    2345678 kB\n";

    std::string p = patch_meminfo(real, 8);
    CHECK(p.find("MemFree:         1234567 kB") != std::string::npos, "meminfo other lines intact");
    CHECK(p.find("MemAvailable:    2345678 kB") != std::string::npos, "meminfo tail intact");
    CHECK(p.rfind("MemTotal:", 0) == 0, "MemTotal still first line");
    uint64_t want8 = ram_gb_to_memtotal_kb(8);
    CHECK(p.find(std::to_string(want8)) != std::string::npos, "8GB memtotal value present");
    CHECK(p.find("7654321") == std::string::npos, "real memtotal replaced");

    std::string d = patch_meminfo(real, 0);
    CHECK(d.find(std::to_string(ram_gb_to_memtotal_kb(8))) != std::string::npos,
          "derive: 7.6GB -> 8GB tier");

    std::string real12 =
        "MemTotal:       11901234 kB\nMemFree: 100 kB\n";
    std::string d12 = patch_meminfo(real12, 0);
    CHECK(d12.find(std::to_string(ram_gb_to_memtotal_kb(12))) != std::string::npos,
          "derive: 11.4GB -> 12GB tier");

    std::string weird = "Foo: 1 kB\nBar: 2 kB\n";
    CHECK(patch_meminfo(weird, 8) == weird, "no MemTotal -> passthrough");

    std::string lead = "\nMemTotal:        7654321 kB\nMemFree: 1 kB\n";
    std::string lp = patch_meminfo(lead, 8);
    CHECK(lp.find(std::to_string(want8)) != std::string::npos, "MemTotal after newline patched");
    CHECK(lp.rfind("\n", 0) == 0, "leading newline preserved");

    CHECK(patch_meminfo(p, 8) == p, "meminfo patch idempotent");
}

static void test_pixel_ram() {
    CHECK(pixel_ram_gb("Pixel 8") == 8, "Pixel 8 = 8GB");
    CHECK(pixel_ram_gb("Pixel 8 Pro") == 12, "Pixel 8 Pro = 12GB");
    CHECK(pixel_ram_gb("Pixel 9 Pro XL") == 16, "Pixel 9 Pro XL = 16GB");
    CHECK(pixel_ram_gb("Pixel 6a") == 6, "Pixel 6a = 6GB");
    CHECK(pixel_ram_gb("Pixel 42") == 0, "unknown model = 0");
    CHECK(pixel_ram_gb("") == 0, "empty model = 0");
}

static void test_cpuinfo() {
    std::string repl;

    int a = cpu_action_for("Qualcomm", "SM8650", repl);
    CHECK(a == CPU_QUALCOMM, "qualcomm manuf -> QUALCOMM");
    CHECK(repl == "Qualcomm Technologies, Inc SM8650", "qualcomm repl string");

    a = cpu_action_for("", "SM7325", repl);
    CHECK(a == CPU_QUALCOMM, "SM prefix -> QUALCOMM");

    a = cpu_action_for("MediaTek", "MT6893", repl);
    CHECK(a == CPU_MTK, "mediatek -> MTK");
    CHECK(repl == "MT6893", "mtk repl string");

    a = cpu_action_for("", "MT6877", repl);
    CHECK(a == CPU_MTK, "MT prefix -> MTK");

    a = cpu_action_for("Google", "Tensor G3", repl);
    CHECK(a == CPU_STRIP, "google/tensor -> STRIP");

    std::string real_qcom =
        "processor\t: 0\n"
        "BogoMIPS\t: 38.40\n"
        "Hardware\t: Qualcomm Technologies, Inc SM_REAL_CHIP\n"
        "Revision\t: 0001\n";
    std::string out;
    cpu_action_for("Qualcomm", "SM8650", repl);
    bool changed = patch_cpuinfo(real_qcom, CPU_QUALCOMM, repl, out);
    CHECK(changed, "qcom cpuinfo changed");
    CHECK(out.find("SM8650") != std::string::npos, "qcom repl applied");
    CHECK(out.find("SM_REAL_CHIP") == std::string::npos, "real chip removed");
    CHECK(out.find("BogoMIPS\t: 38.40") != std::string::npos, "other cpuinfo lines intact");
    CHECK(out.find("Revision\t: 0001") != std::string::npos, "trailing line intact");

    std::string real_pixel =
        "processor\t: 0\n"
        "Hardware\t: Qualcomm Technologies, Inc SM_REAL_CHIP\n"
        "Revision\t: 0001\n";
    std::string outp;
    bool ch2 = patch_cpuinfo(real_pixel, CPU_STRIP, "", outp);
    CHECK(ch2, "strip changed");
    CHECK(outp.find("Hardware") == std::string::npos, "Hardware line stripped");
    CHECK(outp.find("Revision\t: 0001") != std::string::npos, "line after stripped Hardware intact");
    CHECK(outp.find("processor\t: 0") != std::string::npos, "line before stripped Hardware intact");

    std::string no_hw = "processor\t: 0\nBogoMIPS\t: 38.40\n";
    std::string out3;
    bool ch3 = patch_cpuinfo(no_hw, CPU_STRIP, "", out3);
    CHECK(!ch3, "no Hardware -> no change");
    CHECK(out3.empty(), "no-change clears out for passthrough");

    std::string out4;
    CHECK(!patch_cpuinfo(real_qcom, CPU_NONE, "", out4), "CPU_NONE -> passthrough");

    std::string eof_hw = "processor\t: 0\nHardware\t: SM_REAL";
    std::string out5;
    bool ch5 = patch_cpuinfo(eof_hw, CPU_STRIP, "", out5);
    CHECK(ch5, "EOF Hardware line changed");
    CHECK(out5.find("Hardware") == std::string::npos, "EOF Hardware stripped");
    CHECK(out5 == "processor\t: 0\n", "EOF strip leaves preceding lines");
}

static void test_classify() {
    CHECK(classify("/proc/sys/kernel/random/boot_id") == BOOTID, "boot_id classify");
    CHECK(classify("/proc/version") == VERSION, "version classify");
    CHECK(classify("/proc/meminfo") == MEMINFO, "meminfo classify");
    CHECK(classify("/proc/cpuinfo") == CPUINFO, "cpuinfo classify");
    CHECK(classify("/sys/class/net/wlan0/address") == MAC, "wlan0 address classify");
    CHECK(classify("/sys/class/net/wlan1/address") == MAC, "wlan1 address classify");
    CHECK(classify("/sys/class/net/p2p0/address") == MAC, "p2p0 address classify");

    CHECK(classify("/proc/cpuinfo/x") == NONE, "cpuinfo subpath not matched");
    CHECK(classify("/proc/versionx") == NONE, "version prefix not matched");
    CHECK(classify("/sys/class/net/eth0/address") == NONE, "eth0 not matched");
    CHECK(classify("/sys/class/net/wlan0/mtu") == NONE, "wlan0 non-address not matched");
    CHECK(classify("/sys/class/net/wlan0") == NONE, "wlan0 dir not matched");
    CHECK(classify("/proc/self/maps") == NONE, "unrelated proc not matched");
    CHECK(classify("") == NONE, "empty path not matched");
    CHECK(classify(nullptr) == NONE, "null path not matched");
    CHECK(classify("/data/adb/modules/sandboxid/identity.prop") == NONE,
          "identity file not matched");
}

static void test_hex_from_seed() {
    uint64_t seed = fnv1a("google/husky/husky:14/AP1A.240505.004/11583682:user/release-keys");

    std::string d1 = hex_from_seed(seed, 32);
    std::string d2 = hex_from_seed(seed, 32);
    CHECK(d1.size() == 64, "hex_from_seed(32) is 64 chars");
    CHECK(is_hexlc(d1), "hex_from_seed is lowercase hex");
    CHECK(d1 == d2, "hex_from_seed deterministic for same seed");
    CHECK(hex_from_seed(fnv1a("other-identity"), 32) != d1,
          "hex_from_seed differs for different seed");
    CHECK(hex_from_seed(seed, 8).size() == 16, "hex_from_seed(8) is 16 chars");
    CHECK(hex_from_seed(seed, 0).empty(), "hex_from_seed(0) is empty");

    CHECK(hex_from_seed(seed, 16) == d1.substr(0, 32),
          "hex_from_seed prefix stable across lengths");
}

static void test_selinux() {
    CHECK(classify("/sys/fs/selinux/enforce") == SELINUX_ENFORCE, "selinux enforce classify");
    CHECK(classify("/sys/fs/selinux/enforcex") == NONE, "selinux enforce exact-match only");
    CHECK(classify("/sys/fs/selinux/policy") == NONE, "selinux policy not matched");
    CHECK(classify("/sys/fs/selinux/enforce/x") == NONE, "selinux enforce subpath not matched");
    std::string c = selinux_enforce_content();
    CHECK(c == "1", "selinux enforce content is exactly \"1\"");
    CHECK(c.size() == 1, "selinux enforce content is 1 byte (no trailing newline)");
}

static void test_arp() {
    CHECK(classify("/proc/net/arp") == ARP, "arp classify");
    CHECK(classify("/proc/net/arpx") == NONE, "arp exact-match only");
    CHECK(classify("/proc/net/arp/x") == NONE, "arp subpath not matched");
    CHECK(classify("/proc/net/tcp") == NONE, "other /proc/net not matched");
    std::string c = arp_empty_content();
    CHECK(c.rfind("IP address", 0) == 0, "arp content starts with the column header");
    CHECK(c.find("HW address") != std::string::npos, "arp header has HW address column");
    CHECK(!c.empty() && c.back() == '\n', "arp header ends with newline");

    CHECK(std::count(c.begin(), c.end(), '\n') == 1, "arp table is empty (header only)");
}

static void test_hide_prop() {

    CHECK(is_emulator_prop("ro.kernel.qemu"), "ro.kernel.qemu is emulator");
    CHECK(is_emulator_prop("ro.boot.qemu"), "ro.boot.qemu is emulator");
    CHECK(is_emulator_prop("qemu.hw.mainkeys"), "qemu.hw.mainkeys is emulator");
    CHECK(is_emulator_prop("qemu.sf.lcd_density"), "qemu. prefix is emulator");
    CHECK(is_emulator_prop("ro.boot.qemu.avd_name"), "ro.boot.qemu. prefix is emulator");
    CHECK(is_emulator_prop("ro.kernel.qemu.gles"), "ro.kernel.qemu.gles is emulator");
    CHECK(is_emulator_prop("init.svc.qemud"), "init.svc.qemud is emulator");
    CHECK(!is_emulator_prop("ro.product.model"), "real product prop not emulator");
    CHECK(!is_emulator_prop("ro.hardware"), "ro.hardware must NOT be blanked");
    CHECK(!is_emulator_prop("ro.hardware.egl"), "ro.hardware.egl must NOT be blanked");
    CHECK(!is_emulator_prop(""), "empty prop not emulator");
    CHECK(!is_emulator_prop(nullptr), "null prop not emulator");

    CHECK(is_custom_rom_prop("ro.lineage.version"), "ro.lineage.* is custom rom");
    CHECK(is_custom_rom_prop("lineage.updater.uri"), "lineage.* is custom rom");
    CHECK(is_custom_rom_prop("ro.modversion"), "ro.modversion is custom rom");
    CHECK(is_custom_rom_prop("ro.cm.version"), "ro.cm.version is custom rom");
    CHECK(is_custom_rom_prop("persist.sys.lineage.foo"), "persist.sys.lineage.* is custom rom");
    CHECK(!is_custom_rom_prop("ro.build.version.sdk"), "sdk prop not custom rom");
    CHECK(!is_custom_rom_prop("ro.cmdline"), "ro.cmdline not custom rom (ro.cm. must be dotted)");
    CHECK(!is_custom_rom_prop(nullptr), "null prop not custom rom");

    CHECK(should_hide_prop("qemu.hw.mainkeys"), "should_hide covers emulator");
    CHECK(should_hide_prop("ro.lineage.version"), "should_hide covers custom rom");
    CHECK(!should_hide_prop("ro.product.brand"), "should_hide leaves normal props");
}

static void test_applog_classify() {

    CHECK(classify("/data/data/com.zhiliaoapp.musically/shared_prefs/applog.xml")
          == APPLOG_XML, "applog.xml classify");
    CHECK(classify("/data/user/0/com.ss.android.ugc.trill/shared_prefs/applog.xml")
          == APPLOG_XML, "applog.xml via /data/user/0 classify");
    CHECK(classify("/data/data/x/shared_prefs/snssdk_openudid.xml")
          == APPLOG_XML, "snssdk_openudid.xml classify");
    CHECK(classify("/data/data/x/shared_prefs/snssdk_did.xml")
          == APPLOG_XML, "snssdk_did.xml classify");
    CHECK(classify("/data/data/x/shared_prefs/bd_device_info.xml")
          == APPLOG_XML, "bd_device_info.xml classify");

    CHECK(classify("/data/data/x/files/bd_setting/device_id")   == BD_RAW_DID,
          "bd_setting/device_id classify");
    CHECK(classify("/data/data/x/files/bd_setting/install_id")  == BD_RAW_IID,
          "bd_setting/install_id classify");
    CHECK(classify("/data/data/x/files/bd_setting/openudid")    == BD_RAW_OPENUDID,
          "bd_setting/openudid classify");
    CHECK(classify("/data/data/x/files/bd_setting/clientudid")  == BD_RAW_CLIENTUDID,
          "bd_setting/clientudid classify");
    CHECK(classify("/data/data/x/files/.cdid")                  == BD_RAW_CDID,
          "files/.cdid classify");

    // Cakupan diperluas sinkron dengan applog_wipe() (scripts/lib/helpers.sh).
    CHECK(classify("/data/data/x/shared_prefs/applog_stats.xml")
          == APPLOG_XML, "applog_stats.xml classify");
    CHECK(classify("/data/data/x/shared_prefs/header_custom.xml")
          == APPLOG_XML, "header_custom.xml classify");
    CHECK(classify("/data/data/x/shared_prefs/ug_install_settings_pref.xml")
          == APPLOG_XML, "ug_install_settings_pref.xml classify");
    CHECK(classify("/data/data/x/no_backup/applog_device_id.dat") == BD_RAW_DID,
          "no_backup/applog_device_id.dat classify");
    CHECK(classify("/data/data/x/no_backup/bd_device_id") == BD_RAW_DID,
          "no_backup/bd_device_id classify");
    CHECK(classify("/data/data/x/no_backup/.cdid") == BD_RAW_CDID,
          "no_backup/.cdid classify");

    // Gating sintesis: hanya peta ID kanonis yang boleh disintesis dari nol.
    CHECK(applog_xml_is_synthable("/data/data/x/shared_prefs/applog.xml"),
          "applog.xml synthable");
    CHECK(applog_xml_is_synthable("/data/data/x/shared_prefs/applog_stats.xml"),
          "applog_stats.xml synthable");
    CHECK(!applog_xml_is_synthable("/data/data/x/shared_prefs/header_custom.xml"),
          "header_custom.xml NOT synthable (patch-only)");
    CHECK(!applog_xml_is_synthable("/data/data/x/shared_prefs/ug_install_settings_pref.xml"),
          "ug_install_settings_pref.xml NOT synthable (patch-only)");
    CHECK(!applog_xml_is_synthable(nullptr), "null path not synthable");

    CHECK(classify("/data/data/x/shared_prefs/applog_other.xml") == NONE,
          "applog_other.xml not matched");
    CHECK(classify("/data/data/x/files/other.cdid")             == NONE,
          "other.cdid not matched");
    CHECK(classify("/data/data/x/files/bd_setting/other")       == NONE,
          "bd_setting/other not matched");
    CHECK(classify("/data/data/x/shared_prefs/user_prefs.xml")  == NONE,
          "unrelated xml not matched");

    CHECK(classify("/data/data/applog.xml") == NONE, "bare applog.xml not matched");

    CHECK(classify("/proc/version") == VERSION, "version still classifies");
    CHECK(classify("/proc/meminfo") == MEMINFO, "meminfo still classifies");
}

static void test_applog_ids() {

    uint64_t epoch = 1710000000000ULL;
    uint64_t seed  = fnv1a("google/husky/husky:14/AP1A.240505.004/11583682:user/release-keys|com.zhiliaoapp.musically");

    ApplogIds a = make_applog_ids(seed, epoch);

    for (const std::string* id : {&a.did, &a.iid, &a.ssid}) {
        CHECK(id->size() == 19, "snowflake id is 19 decimal digits");
        unsigned long long v = std::strtoull(id->c_str(), nullptr, 10);
        CHECK((v >> 22) == epoch, "snowflake decodes to epoch_ms");
        CHECK(v < 9223372036854775807ULL, "snowflake fits positive int64 (Java long)");
        for (char c : *id) CHECK(c >= '0' && c <= '9', "snowflake is decimal digits only");
    }
    CHECK(a.did != a.iid && a.did != a.ssid && a.iid != a.ssid,
          "did/iid/ssid distinct");

    for (const std::string* u : {&a.cdid, &a.clientudid}) {
        CHECK(u->size() == 36, "uuid length 36");
        CHECK((*u)[14] == '4', "uuid version nibble == 4");
        char var = (*u)[19];
        CHECK(var == '8' || var == '9' || var == 'a' || var == 'b',
              "uuid variant nibble in {8,9,a,b}");
    }
    CHECK(a.cdid != a.clientudid, "cdid and clientudid distinct");

    CHECK(a.openudid.size() == 16, "openudid is 16 chars");
    CHECK(is_hexlc(a.openudid), "openudid is lowercase hex");

    ApplogIds a2 = make_applog_ids(seed, epoch);
    CHECK(a2.did == a.did && a2.iid == a.iid && a2.ssid == a.ssid &&
          a2.cdid == a.cdid && a2.clientudid == a.clientudid &&
          a2.openudid == a.openudid, "same (seed, epoch) -> same ids");

    ApplogIds other_pkg = make_applog_ids(fnv1a("fp|serial|aid|com.ss.android.ugc.trill"), epoch);
    CHECK(other_pkg.did != a.did, "per-package seed -> different did");

    ApplogIds bumped = make_applog_ids(seed, epoch + 86400000ULL);
    CHECK(bumped.did != a.did, "epoch bump -> different did");
    CHECK((std::strtoull(bumped.did.c_str(), nullptr, 10) >> 22) == epoch + 86400000ULL,
          "bumped did decodes to new epoch");
}

static void test_applog_xml_patch() {
    uint64_t epoch = 1710000000000ULL;
    ApplogIds a = make_applog_ids(fnv1a("persona|com.zhiliaoapp.musically"), epoch);

    std::string real =
        "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n"
        "<map>\n"
        "    <string name=\"header_device_id\">6990234216324986369</string>\n"
        "    <string name=\"install_id\">7137846409338136325</string>\n"
        "    <string name=\"OpenUDID\">5aeca2e40e7e5cf2</string>\n"
        "    <string name=\"ab_test\">keep-me-verbatim</string>\n"
        "    <long name=\"register_time\" value=\"1663094970000\" />\n"
        "    <string name=\"clientudid\">a041f068-1eb3-41b8-8442-0d92622d4b4d</string>\n"
        "    <string name=\"cdid\">f8741d6d-78c5-4d95-983c-54ef73e284f7</string>\n"
        "    <string name=\"ssid\">7123456789012345678</string>\n"
        "</map>\n";

    std::string out;
    CHECK(patch_applog_xml(real, a, out), "valid applog xml patched");

    CHECK(out.find(">" + a.did + "<")        != std::string::npos, "did replaced");
    CHECK(out.find(">" + a.iid + "<")        != std::string::npos, "iid replaced");
    CHECK(out.find(">" + a.openudid + "<")   != std::string::npos, "openudid replaced");
    CHECK(out.find(">" + a.clientudid + "<") != std::string::npos, "clientudid replaced");
    CHECK(out.find(">" + a.cdid + "<")       != std::string::npos, "cdid replaced");
    CHECK(out.find(">" + a.ssid + "<")       != std::string::npos, "ssid replaced");

    CHECK(out.find("keep-me-verbatim")       != std::string::npos, "unrelated string entry kept");
    CHECK(out.find("1663094970000")          != std::string::npos, "long entry kept");
    CHECK(out.find("ab_test")                != std::string::npos, "ab_test key kept");
    CHECK(out.find("6990234216324986369")    == std::string::npos, "old did gone");
    CHECK(out.find("7137846409338136325")    == std::string::npos, "old iid gone");
    CHECK(out.find("5aeca2e40e7e5cf2")       == std::string::npos, "old openudid gone");
    CHECK(out.find("a041f068-1eb3-41b8-8442-0d92622d4b4d") == std::string::npos,
          "old clientudid gone");
    CHECK(out.find("register_time")          != std::string::npos, "register_time key kept");
    CHECK(out.rfind("</map>", out.size() - 2) != std::string::npos, "closing map intact");

    std::string out2;
    CHECK(patch_applog_xml(out, a, out2) && out2 == out, "patch is idempotent");

    std::string o;
    CHECK(!patch_applog_xml("hello world", a, o), "plain text rejected");
    CHECK(!patch_applog_xml("<html><body>x</body></html>", a, o), "non-map xml rejected");
    CHECK(!patch_applog_xml("", a, o), "empty input rejected");

    std::string nokeys =
        "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n"
        "<map>\n    <string name=\"lang\">en</string>\n</map>\n";
    std::string pk;
    CHECK(patch_applog_xml(nokeys, a, pk) && pk == nokeys, "no-id map passes through");
}

static bool vec_has(const std::vector<std::string>& v, const char* s) {
    for (const auto& e : v) if (e == s) return true;
    return false;
}

static void test_mountinfo() {
    using namespace sbxmnt;

    MountRow r;
    bool ok = parse_mountinfo_line(
        "50 30 0:33 / /data/adb/magisk rw,relatime shared:2 master:1 - ext4 /dev/block/dm-1 rw", r);
    CHECK(ok, "mountinfo line parses with optional fields");
    CHECK(r.mount_point == "/data/adb/magisk", "parsed mount point");
    CHECK(r.fstype == "ext4", "parsed fstype after separator");
    CHECK(r.source == "/dev/block/dm-1", "parsed source after fstype");
    CHECK(!parse_mountinfo_line("garbage short line", r), "short line rejected");
    CHECK(!parse_mountinfo_line("", r), "empty line rejected");

    std::string mi =
        "10 1 0:1 / / rw shared:1 - ext4 /dev/root rw\n"
        "30 10 0:21 / /data rw - ext4 /dev/block/dm-2 rw\n"
        "50 30 0:33 / /data/adb/magisk rw - ext4 /dev/block/dm-1 rw\n"
        "60 30 0:34 /adb/modules/sandboxid /data/adb/modules/sandboxid rw - ext4 /dev/block/dm-1 rw\n"
        "70 30 0:35 /adb/mod/mount/system/build.prop /system/build.prop rw - ext4 /dev/block/dm-1 rw\n"
        "80 30 0:36 / /system/etc/hosts rw - overlay magisk rw\n"
        "85 1 0:40 / /vendor rw - ext4 /dev/block/dm-9 rw\n"
        "90 1 0:44 / /mnt/user rw - fuse /dev/fuse rw\n"
        "this is a malformed line\n"
        "95 1 0:12 / /debug_ramdisk rw - tmpfs tmpfs rw\n";

    std::vector<std::string> t = select_umount_targets(mi);

    CHECK(t.size() == 3, "exactly three traces selected");
    CHECK(vec_has(t, "/data/adb/magisk"), "magisk under /data/adb selected");
    CHECK(vec_has(t, "/system/etc/hosts"), "magisk overlay selected");
    CHECK(vec_has(t, "/debug_ramdisk"), "debug_ramdisk selected");

    CHECK(!vec_has(t, "/system/build.prop"), "our persona build.prop bind protected");
    CHECK(!vec_has(t, "/data/adb/modules/sandboxid"), "our module tree protected");
    CHECK(!vec_has(t, "/data"), "/data never unmounted");
    CHECK(!vec_has(t, "/"), "/ never unmounted");
    CHECK(!vec_has(t, "/vendor"), "bare /vendor root never unmounted");
    CHECK(!vec_has(t, "/mnt/user"), "unrelated fuse mount left alone");

    CHECK(t.front() == "/debug_ramdisk", "reverse order: last trace first");
    CHECK(t.back() == "/data/adb/magisk", "reverse order: first trace last");

    CHECK(select_umount_targets("").empty(), "empty mountinfo -> no targets");
}

static void test_native_unsafe_prop() {
    const char* unsafe[] = {
        "ro.hardware", "ro.product.board", "ro.board.platform", "ro.arch",
        "ro.zygote", "ro.vendor.api_level", "persist.graphics.egl",
        "ro.product.cpu.abi", "ro.product.cpu.abi2", "ro.product.cpu.abilist",
        "ro.product.cpu.abilist32", "ro.product.cpu.abilist64",
        "ro.hardware.egl", "ro.hardware.vulkan", "ro.hardware.gralloc",
        "ro.hardware.hwcomposer", "ro.hardware.camera",
        "ro.dalvik.vm.isa.arm", "dalvik.vm.isa.arm.features",
    };
    for (const char* p : unsafe)
        CHECK(is_native_unsafe_prop(p), p);

    const char* spoofable[] = {
        "ro.product.model", "ro.product.brand", "ro.product.manufacturer",
        "ro.product.device", "ro.product.name", "ro.build.fingerprint",
        "ro.build.id", "ro.build.version.release", "ro.build.version.sdk",
        "ro.soc.manufacturer", "ro.soc.model", "ro.serialno",
        "gsm.operator.numeric", "persist.sys.timezone",
        "dalvik.vm.heapgrowthlimit", "ro.hardwaremodel", "ro.arch2",
    };
    for (const char* p : spoofable)
        CHECK(!is_native_unsafe_prop(p), p);

    CHECK(!is_native_unsafe_prop(nullptr), "null prop name is not unsafe");
    CHECK(!is_native_unsafe_prop(""), "empty prop name is not unsafe");
}

static void test_classify_no_alloc_paths() {
    CHECK(classify("/data/data/com.ss.android.ugc.trill/shared_prefs/applog.xml") == APPLOG_XML,
          "applog.xml classified");
    CHECK(classify("/data/user/0/com.zhiliaoapp.musically/files/bd_setting/device_id") == BD_RAW_DID,
          "bd_setting/device_id classified");
    CHECK(classify("applog.xml") == NONE, "bare applog.xml not classified");
    CHECK(classify("/x") == NONE, "short path not classified");
    CHECK(classify("") == NONE, "empty path not classified");
}

static void test_applog_xml_synth() {
    ApplogIds ids = make_applog_ids(fnv1a("seed|synth|pkg"), 1727839200000ULL);
    std::string x = applog_xml_synth(ids);
    CHECK(x.find("<map>") != std::string::npos, "synth has <map>");
    CHECK(x.find("</map>") != std::string::npos, "synth has </map>");
    CHECK(x.find(ids.did) != std::string::npos, "synth carries did");
    CHECK(x.find(ids.iid) != std::string::npos, "synth carries iid");
    CHECK(x.find(ids.ssid) != std::string::npos, "synth carries ssid");
    CHECK(x.find(ids.openudid) != std::string::npos, "synth carries openudid");
    CHECK(x.find(ids.clientudid) != std::string::npos, "synth carries clientudid");
    CHECK(x.find(ids.cdid) != std::string::npos, "synth carries cdid");

    ApplogIds other = make_applog_ids(fnv1a("seed|synth|other"), 1727839200000ULL);
    std::string patched;
    CHECK(patch_applog_xml(x, other, patched), "synth output is patchable");
    CHECK(patched.find(other.did) != std::string::npos, "patch replaced did");
    CHECK(patched.find(ids.did) == std::string::npos, "old did gone after patch");
    CHECK(patched.find(other.cdid) != std::string::npos, "patch replaced cdid");

    std::string twice;
    CHECK(patch_applog_xml(patched, other, twice), "patch is idempotent");
    CHECK(twice == patched, "second patch is a no-op");
}

int main() {
    test_uuid();
    test_mac();
    test_mac_bytes_and_iface();
    test_proc_version();
    test_meminfo();
    test_pixel_ram();
    test_cpuinfo();
    test_classify();
    test_hex_from_seed();
    test_selinux();
    test_arp();
    test_hide_prop();
    test_native_unsafe_prop();
    test_classify_no_alloc_paths();
    test_applog_classify();
    test_applog_ids();
    test_applog_xml_patch();
    test_applog_xml_synth();
    test_mountinfo();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
