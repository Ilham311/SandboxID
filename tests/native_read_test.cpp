// tests/native_read_test.cpp — host unit tests for sbx_native_read.hpp.
//
// Build & run (mirrors the module's release warning flags):
//   c++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
//       tests/native_read_test.cpp -o /tmp/nrt && /tmp/nrt
//
// Pure logic only: no JNI/Zygisk/syscalls are exercised here (those live in
// main.cpp and can only be verified on-device after a CI rebuild).

#include "../jni/sbx_native_read.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using namespace sbxnr;

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond, msg) do {                                            \
    ++g_checks;                                                          \
    if (!(cond)) { ++g_fails; std::printf("FAIL: %s  (%s:%d)\n",         \
                                          (msg), __FILE__, __LINE__); }  \
} while (0)

// ---- helpers ----
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
    // hex-only between dashes
    std::string stripped;
    for (char c : u1) if (c != '-') stripped.push_back(c);
    CHECK(stripped.size() == 32 && is_hexlc(stripped), "uuid body is 32 lowercase hex");
    // different identity -> different uuid
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

static void test_proc_version() {
    // Pixel 8 (husky) = Tensor gs201? No: husky=Pixel 8 is zuma. Use zuma.
    std::string v = synth_proc_version("14", "11583682", "zuma", "abfarm42", 0x1234abcd);
    CHECK(v.rfind("Linux version ", 0) == 0, "version starts 'Linux version '");
    CHECK(v.find("-android14-") != std::string::npos, "android14 token present");
    CHECK(v.find("5.15.") != std::string::npos, "zuma -> kernel 5.15");
    CHECK(v.find("clang version 17.0.4") != std::string::npos, "A14 -> clang 17.0.4");
    CHECK(v.find("#1 SMP PREEMPT") != std::string::npos, "SMP PREEMPT present");
    CHECK(v.find("abfarm42") != std::string::npos, "build host present");

    // determinism
    CHECK(synth_proc_version("14", "11583682", "zuma", "abfarm42", 0x1234abcd) == v,
          "proc/version deterministic");

    // platform overrides release for kernel base
    std::string g6 = synth_proc_version("14", "123456", "gs101", "", 42);
    CHECK(g6.find("5.10.") != std::string::npos, "gs101 -> kernel 5.10 even on A14");

    // release drives kernel when platform unknown
    std::string a15 = synth_proc_version("15", "999999", "", "", 7);
    CHECK(a15.find("6.1.") != std::string::npos, "A15 no-platform -> kernel 6.1");
    CHECK(a15.find("clang version 18.0.1") != std::string::npos, "A15 -> clang 18.0.1");
    CHECK(a15.find("-android15-") != std::string::npos, "android15 token");

    // zumapro -> 6.1
    std::string zp = synth_proc_version("15", "888888", "zumapro", "", 5);
    CHECK(zp.find("6.1.") != std::string::npos, "zumapro -> kernel 6.1");

    // empty release still produces a plausible line (fallback android14 / 5.15)
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

    // explicit Pixel 8 = 8 GB
    std::string p = patch_meminfo(real, 8);
    CHECK(p.find("MemFree:         1234567 kB") != std::string::npos, "meminfo other lines intact");
    CHECK(p.find("MemAvailable:    2345678 kB") != std::string::npos, "meminfo tail intact");
    CHECK(p.rfind("MemTotal:", 0) == 0, "MemTotal still first line");
    uint64_t want8 = ram_gb_to_memtotal_kb(8);
    CHECK(p.find(std::to_string(want8)) != std::string::npos, "8GB memtotal value present");
    CHECK(p.find("7654321") == std::string::npos, "real memtotal replaced");

    // derive from real (7.6 GB rounds up to 8 GB tier)
    std::string d = patch_meminfo(real, 0);
    CHECK(d.find(std::to_string(ram_gb_to_memtotal_kb(8))) != std::string::npos,
          "derive: 7.6GB -> 8GB tier");

    // 11.4 GB rounds up to 12
    std::string real12 =
        "MemTotal:       11901234 kB\nMemFree: 100 kB\n";
    std::string d12 = patch_meminfo(real12, 0);
    CHECK(d12.find(std::to_string(ram_gb_to_memtotal_kb(12))) != std::string::npos,
          "derive: 11.4GB -> 12GB tier");

    // no MemTotal line -> unchanged
    std::string weird = "Foo: 1 kB\nBar: 2 kB\n";
    CHECK(patch_meminfo(weird, 8) == weird, "no MemTotal -> passthrough");

    // MemTotal not at column 0 originally (leading blank line) still handled
    std::string lead = "\nMemTotal:        7654321 kB\nMemFree: 1 kB\n";
    std::string lp = patch_meminfo(lead, 8);
    CHECK(lp.find(std::to_string(want8)) != std::string::npos, "MemTotal after newline patched");
    CHECK(lp.rfind("\n", 0) == 0, "leading newline preserved");

    // idempotence: patching an already-patched buffer to same target is stable
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

    // Qualcomm by manufacturer
    int a = cpu_action_for("Qualcomm", "SM8650", repl);
    CHECK(a == CPU_QUALCOMM, "qualcomm manuf -> QUALCOMM");
    CHECK(repl == "Qualcomm Technologies, Inc SM8650", "qualcomm repl string");

    // Qualcomm by model prefix only
    a = cpu_action_for("", "SM7325", repl);
    CHECK(a == CPU_QUALCOMM, "SM prefix -> QUALCOMM");

    // MediaTek
    a = cpu_action_for("MediaTek", "MT6893", repl);
    CHECK(a == CPU_MTK, "mediatek -> MTK");
    CHECK(repl == "MT6893", "mtk repl string");

    // MT prefix only
    a = cpu_action_for("", "MT6877", repl);
    CHECK(a == CPU_MTK, "MT prefix -> MTK");

    // Google/Tensor -> strip
    a = cpu_action_for("Google", "Tensor G3", repl);
    CHECK(a == CPU_STRIP, "google/tensor -> STRIP");

    // --- patching ---
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

    // strip: Hardware line removed entirely, tail preserved
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

    // no Hardware line -> passthrough (returns false, out cleared)
    std::string no_hw = "processor\t: 0\nBogoMIPS\t: 38.40\n";
    std::string out3;
    bool ch3 = patch_cpuinfo(no_hw, CPU_STRIP, "", out3);
    CHECK(!ch3, "no Hardware -> no change");
    CHECK(out3.empty(), "no-change clears out for passthrough");

    // CPU_NONE -> passthrough
    std::string out4;
    CHECK(!patch_cpuinfo(real_qcom, CPU_NONE, "", out4), "CPU_NONE -> passthrough");

    // Hardware line with no trailing newline (EOF) still handled
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

int main() {
    test_uuid();
    test_mac();
    test_proc_version();
    test_meminfo();
    test_pixel_ram();
    test_cpuinfo();
    test_classify();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
