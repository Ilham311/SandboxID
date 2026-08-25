#pragma once
//
// sbx_native_read.hpp — PURE content logic for the native/file read-spoof layer
// (L9). NO JNI, NO Zygisk, NO syscalls: everything here is a deterministic string
// transform so it can be unit-tested on the host (see tests/native_read_test.cpp)
// with the same -Wall -Wextra -Werror -fno-exceptions -fno-rtti flags the module
// ships with. main.cpp owns the actual hooks (PLT install, memfd, /proc reads) and
// calls into these builders.
//
// Why native at all: the L2 hook only covers android.os.SystemProperties.native_get
// (the *Java* path). Native code calling __system_property_get() directly, and any
// code that reads /proc or /sys as plain files, bypasses every Java-side hook. This
// layer closes those: native property reads return the SAME value as L2, and a
// handful of high-signal pseudo-files (/proc/version, boot_id, sysfs Wi-Fi MAC,
// /proc/meminfo, /proc/cpuinfo Hardware) are rewritten to match the spoofed device.
//
// All fabricated values are derived DETERMINISTICALLY from the already-present
// identity (FINGERPRINT/SERIAL/RELEASE/…), so they are (a) stable across every read
// and every target app for one identity, (b) different from the real device, and
// (c) rotate automatically when the identity rotates — no new identity.prop key and
// no shell/WebUI change required. WIFI_MAC is the one exception: when the shell has
// already persisted one (helpers.sh/rotate_ids.sh) we reuse it verbatim.

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>

namespace sbxnr {

// ---------------------------------------------------------------------------
// Deterministic hashing / byte expansion. FNV-1a (64) for the seed, splitmix64
// to expand into as many pseudo-random-but-reproducible bytes as we need. No
// std::random here: identical identity => identical boot_id/MAC across processes.
// ---------------------------------------------------------------------------
inline uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

inline uint64_t splitmix64(uint64_t& x) {
    x += 0x9E3779B97F4A7C15ULL;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

inline void fill_bytes(uint64_t seed, uint8_t* out, size_t n) {
    uint64_t s = seed;
    size_t i = 0;
    while (i < n) {
        uint64_t v = splitmix64(s);
        for (int b = 0; b < 8 && i < n; ++b, ++i)
            out[i] = static_cast<uint8_t>(v >> (b * 8));
    }
}

inline char hex_lc(unsigned v) { return "0123456789abcdef"[v & 0xF]; }

// ---------------------------------------------------------------------------
// boot_id: a v4-shaped UUID. /proc/sys/kernel/random/boot_id is a per-boot random
// UUID; leaving the real one exposed lets two "different" spoofed apps be tied to
// the same physical boot. We replace it with a stable, identity-derived UUID.
// ---------------------------------------------------------------------------
inline std::string uuid_from_seed(uint64_t seed) {
    uint8_t b[16];
    fill_bytes(seed, b, sizeof(b));
    b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);        // version 4
    b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);        // variant 10x
    std::string s;
    s.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        s.push_back(hex_lc(b[i] >> 4));
        s.push_back(hex_lc(b[i] & 0xF));
    }
    return s;
}

// ---------------------------------------------------------------------------
// Wi-Fi MAC. Format matches helpers.sh generate_mac(): a locally-administered
// 02:xx:xx:xx:xx:xx address (fixed 0x02 first octet, remainder identity-derived).
// Only used when the shell has NOT already persisted WIFI_MAC.
// ---------------------------------------------------------------------------
inline std::string mac_from_seed(uint64_t seed) {
    uint8_t b[6];
    fill_bytes(seed, b, sizeof(b));
    b[0] = 0x02;                                             // locally administered, unicast
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5]);
    return std::string(buf);
}

// Accept a persisted MAC only if it is a well-formed, non-null, unicast address.
inline bool is_valid_mac(const std::string& m) {
    if (m.size() != 17) return false;
    for (int i = 0; i < 17; ++i) {
        char c = m[i];
        if ((i % 3) == 2) { if (c != ':') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    // reject 00:00:00:00:00:00 (the "unavailable" sentinel) and broadcast.
    bool all_zero = true;
    for (char c : m) if (c != '0' && c != ':') { all_zero = false; break; }
    return !all_zero;
}

// ---------------------------------------------------------------------------
// /proc/version. The real string leaks the true kernel version, build host and
// build user — a strong hardware/ROM fingerprint. We synthesise a plausible
// Google-style GKI line whose kernel base matches the spoofed Tensor generation
// (or, for non-Pixel personas, the Android release) and whose android<N> token
// matches RELEASE. Sub-version / build tags are identity-derived, not attestable.
// ---------------------------------------------------------------------------
inline int release_major(const std::string& release) {
    return std::atoi(release.c_str());   // "14", "14.0.1" -> 14; "" -> 0
}

// Kernel base ("5.10" / "5.15" / "6.1") for a given Tensor platform, else derived
// from the Android release. Pixel kernels track the SoC generation, not the OS
// version (a Pixel 6 stays on 5.10 across Android 12/13/14), so platform wins.
inline const char* kernel_base(const std::string& platform, int rmaj) {
    if (platform == "gs101" || platform == "gs201") return "5.10";
    if (platform == "zuma")                          return "5.15";
    if (platform == "zumapro" || platform == "laguna") return "6.1";
    if (rmaj <= 0)  return "5.15";
    if (rmaj <= 12) return "5.10";
    if (rmaj <= 14) return "5.15";
    return "6.1";                                    // 15, 16, and newer
}

inline const char* clang_for(int rmaj) {
    if (rmaj <= 12) return "14.0.6";
    if (rmaj == 13) return "16.0.2";
    if (rmaj == 14) return "17.0.4";
    return "18.0.1";                                 // 15, 16, and newer
}

inline std::string synth_proc_version(const std::string& release,
                                      const std::string& incremental,
                                      const std::string& platform,
                                      const std::string& host,
                                      uint64_t seed) {
    int rmaj = release_major(release);
    int aver = rmaj > 0 ? rmaj : 14;
    const char* kbase = kernel_base(platform, rmaj);
    const char* clang = clang_for(rmaj);

    unsigned ksub = 100u + static_cast<unsigned>(seed % 120u);        // .100 .. .219
    unsigned krev = 1u + static_cast<unsigned>((seed >> 8) % 15u);    // -1 .. -15

    char ghash[13];
    uint64_t gh = splitmix64(seed);
    for (int i = 0; i < 12; ++i) ghash[i] = hex_lc(static_cast<unsigned>(gh >> (i * 4)));
    ghash[12] = '\0';

    // ab<build>: prefer the real incremental digits (already a build number), else
    // derive one so the field is always present and plausible.
    std::string ab;
    for (char c : incremental) if (c >= '0' && c <= '9') ab.push_back(c);
    if (ab.size() < 6 || ab.size() > 12) {
        char b[16];
        std::snprintf(b, sizeof(b), "%08llu",
                      static_cast<unsigned long long>(10000000ULL + (seed % 90000000ULL)));
        ab = b;
    }

    std::string bhost = host.empty() ? std::string("abfarm-release-01") : host;

    char out[512];
    std::snprintf(out, sizeof(out),
        "Linux version %s.%u-android%d-%u-g%s-ab%s (kleaf@%s) "
        "(Android (based on r522817) clang version %s, LLD 18.0.1) "
        "#1 SMP PREEMPT Mon Jan 1 00:00:00 UTC 2024",
        kbase, ksub, aver, krev, ghash, ab.c_str(), bhost.c_str(), clang);
    return std::string(out);
}

// ---------------------------------------------------------------------------
// /proc/meminfo — MemTotal only. Real MemTotal is a fine device-class fingerprint.
// If the persona is a Pixel we know the RAM tier; otherwise we snap the real total
// UP to the nearest marketing capacity (entropy reduction that never claims a wildly
// different amount). Every other line passes through byte-for-byte.
// ---------------------------------------------------------------------------

// Marketing RAM (GB) for known Pixel MODELs; 0 => unknown (round the real total).
inline int pixel_ram_gb(const std::string& model) {
    struct M { const char* model; int gb; };
    static const M tbl[] = {
        {"Pixel 6", 8}, {"Pixel 6 Pro", 12}, {"Pixel 6a", 6},
        {"Pixel 7", 8}, {"Pixel 7 Pro", 12}, {"Pixel 7a", 8},
        {"Pixel 8", 8}, {"Pixel 8 Pro", 12}, {"Pixel 8a", 8},
        {"Pixel 9", 12}, {"Pixel 9 Pro", 16}, {"Pixel 9 Pro XL", 16},
        {"Pixel 9 Pro Fold", 16}, {"Pixel 9a", 8},
        {"Pixel 10", 12}, {"Pixel 10 Pro", 16}, {"Pixel 10 Pro XL", 16},
        {"Pixel Fold", 12},
    };
    for (const auto& e : tbl) if (model == e.model) return e.gb;
    return 0;
}

// GB -> a realistic MemTotal in kB. Real devices report ~92-96% of the marketing
// capacity (kernel/firmware reserve); 0.955 lands inside that band.
inline uint64_t ram_gb_to_memtotal_kb(int gb) {
    return static_cast<uint64_t>(gb) * 1024ULL * 1024ULL * 955ULL / 1000ULL;
}

inline uint64_t round_up_marketing_gb(uint64_t real_kb) {
    static const int tiers[] = {2, 3, 4, 6, 8, 12, 16, 18, 24};
    // ceil(real_kb / 1 GiB), then snap up to the nearest marketing tier.
    uint64_t gib = (real_kb + 1048575ULL) / 1048576ULL;
    for (int t : tiers) if (static_cast<uint64_t>(t) >= gib) return static_cast<uint64_t>(t);
    return gib;   // absurdly large: leave as-is
}

// Rewrite the "MemTotal:" line of `real`. target_gb 0 => derive from the real total.
// If no MemTotal line exists, returns `real` unchanged.
inline std::string patch_meminfo(const std::string& real, int target_gb) {
    size_t pos = real.find("MemTotal:");
    if (pos != 0 && (pos == std::string::npos || real[pos - 1] != '\n')) {
        // "MemTotal:" must start a line.
        size_t p = real.find("\nMemTotal:");
        if (p == std::string::npos) return real;
        pos = p + 1;
    }
    size_t eol = real.find('\n', pos);
    if (eol == std::string::npos) eol = real.size();

    uint64_t target_kb;
    if (target_gb > 0) {
        target_kb = ram_gb_to_memtotal_kb(target_gb);
    } else {
        // parse the real value to know which tier to round up to.
        uint64_t real_kb = 0;
        const char* p = real.c_str() + pos;
        while (*p && (*p < '0' || *p > '9')) ++p;
        real_kb = std::strtoull(p, nullptr, 10);
        if (real_kb == 0) return real;
        target_kb = ram_gb_to_memtotal_kb(static_cast<int>(round_up_marketing_gb(real_kb)));
    }

    char line[64];
    std::snprintf(line, sizeof(line), "MemTotal:       %llu kB",
                  static_cast<unsigned long long>(target_kb));
    std::string out;
    out.reserve(real.size() + 8);
    out.append(real, 0, pos);
    out.append(line);
    out.append(real, eol, std::string::npos);
    return out;
}

// ---------------------------------------------------------------------------
// /proc/cpuinfo — the "Hardware" line. On arm64 Pixels there is NO Hardware line;
// Qualcomm/MediaTek devices expose one that names the real SoC. So: for a Pixel
// (Google/Tensor) persona we STRIP any Hardware line the real device has; for a
// Qualcomm/MediaTek persona we REWRITE it to a correctly-shaped SoC string. Every
// other line passes through. If there is nothing to do we signal passthrough so the
// caller serves the real fd (avoids a needless, /proc/self/fd-detectable memfd).
// ---------------------------------------------------------------------------
enum CpuAction { CPU_NONE = 0, CPU_QUALCOMM = 1, CPU_MTK = 2, CPU_STRIP = 3 };

inline bool ci_contains(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

inline bool starts_with(const std::string& s, const char* p) {
    size_t n = std::strlen(p);
    return s.size() >= n && std::memcmp(s.data(), p, n) == 0;
}

// Decide how a persona's SoC should present in /proc/cpuinfo, and (for the rewrite
// cases) the replacement Hardware value.
inline int cpu_action_for(const std::string& soc_manuf, const std::string& soc_model,
                          std::string& repl_out) {
    repl_out.clear();
    bool qcom = ci_contains(soc_manuf, "qualcomm") ||
                starts_with(soc_model, "SM") || starts_with(soc_model, "MSM") ||
                starts_with(soc_model, "SDM") || starts_with(soc_model, "QCM") ||
                starts_with(soc_model, "APQ");
    bool mtk  = ci_contains(soc_manuf, "mediatek") || starts_with(soc_model, "MT");
    if (qcom) {
        repl_out = soc_model.empty() ? std::string("Qualcomm Technologies, Inc")
                                     : ("Qualcomm Technologies, Inc " + soc_model);
        return CPU_QUALCOMM;
    }
    if (mtk) {
        repl_out = soc_model.empty() ? std::string("MT6893") : soc_model;
        return CPU_MTK;
    }
    // Google/Tensor, Exynos, Kirin, or unknown: real Pixels have no Hardware line,
    // so the coherent move for the common (Pixel) persona is to strip it.
    return CPU_STRIP;
}

// Returns false => passthrough (nothing changed / no Hardware line to act on).
inline bool patch_cpuinfo(const std::string& real, int action,
                          const std::string& repl, std::string& out) {
    if (action == CPU_NONE) return false;
    out.clear();
    out.reserve(real.size() + 16);
    bool changed = false;
    size_t i = 0, n = real.size();
    while (i < n) {
        size_t eol = real.find('\n', i);
        size_t line_end = (eol == std::string::npos) ? n : eol;
        // A "Hardware" line: token "Hardware" followed by optional ws then ':'.
        bool is_hw = false;
        if (line_end - i >= 8 && std::memcmp(real.data() + i, "Hardware", 8) == 0) {
            size_t j = i + 8;
            while (j < line_end && (real[j] == ' ' || real[j] == '\t')) ++j;
            if (j < line_end && real[j] == ':') is_hw = true;
        }
        if (is_hw) {
            changed = true;
            if (action != CPU_STRIP) {
                out.append("Hardware\t: ");
                out.append(repl);
                if (eol != std::string::npos) out.push_back('\n');
            }
            // CPU_STRIP: drop the line entirely (including its newline).
        } else {
            out.append(real, i, line_end - i);
            if (eol != std::string::npos) out.push_back('\n');
        }
        if (eol == std::string::npos) break;
        i = eol + 1;
    }
    if (!changed) { out.clear(); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Path classification. Only exact, absolute pseudo-file paths are intercepted.
// ---------------------------------------------------------------------------
enum Kind { NONE = 0, BOOTID, MAC, VERSION, MEMINFO, CPUINFO };

inline Kind classify(const char* path) {
    if (!path) return NONE;
    if (std::strcmp(path, "/proc/sys/kernel/random/boot_id") == 0) return BOOTID;
    if (std::strcmp(path, "/proc/version") == 0) return VERSION;
    if (std::strcmp(path, "/proc/meminfo") == 0) return MEMINFO;
    if (std::strcmp(path, "/proc/cpuinfo") == 0) return CPUINFO;

    // /sys/class/net/<iface>/address, iface in {wlan*, p2p*} (Wi-Fi only).
    static const char pfx[] = "/sys/class/net/";
    const size_t pl = sizeof(pfx) - 1;
    if (std::strncmp(path, pfx, pl) == 0) {
        const char* rest = path + pl;
        const char* slash = std::strchr(rest, '/');
        if (slash && std::strcmp(slash, "/address") == 0) {
            size_t iflen = static_cast<size_t>(slash - rest);
            if ((iflen >= 4 && std::strncmp(rest, "wlan", 4) == 0) ||
                (iflen >= 3 && std::strncmp(rest, "p2p", 3) == 0))
                return MAC;
        }
    }
    return NONE;
}

}  // namespace sbxnr
