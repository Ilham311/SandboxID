#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>

namespace sbxnr {

inline uint64_t fnv1a(const std::string& s) {
    uint64_t h = 14695981039346656037ULL;
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

inline std::string uuid_from_seed(uint64_t seed) {
    uint8_t b[16];
    fill_bytes(seed, b, sizeof(b));
    b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);
    std::string s;
    s.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        s.push_back(hex_lc(b[i] >> 4));
        s.push_back(hex_lc(b[i] & 0xF));
    }
    return s;
}

inline std::string mac_from_seed(uint64_t seed) {
    uint8_t b[6];
    fill_bytes(seed, b, sizeof(b));
    b[0] = 0x02;
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5]);
    return std::string(buf);
}

inline bool is_valid_mac(const std::string& m) {
    if (m.size() != 17) return false;
    for (int i = 0; i < 17; ++i) {
        char c = m[i];
        if ((i % 3) == 2) { if (c != ':') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }

    bool all_zero = true;
    for (char c : m) if (c != '0' && c != ':') { all_zero = false; break; }
    return !all_zero;
}

inline std::string hex_from_seed(uint64_t seed, size_t nbytes) {
    std::string s;
    s.reserve(nbytes * 2);
    uint64_t st = seed;
    size_t i = 0;
    while (i < nbytes) {
        uint64_t v = splitmix64(st);
        for (int b = 0; b < 8 && i < nbytes; ++b, ++i) {
            uint8_t byte = static_cast<uint8_t>(v >> (b * 8));
            s.push_back(hex_lc(byte >> 4));
            s.push_back(hex_lc(byte & 0xF));
        }
    }
    return s;
}

struct ApplogIds {
    std::string did, iid, ssid, cdid, clientudid, openudid;
};

inline std::string snowflake_from_seed(uint64_t seed, uint64_t epoch_ms) {

    uint64_t v = (epoch_ms << 22) | (splitmix64(seed) & 0x3FFFFFULL);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    return std::string(buf);
}

inline ApplogIds make_applog_ids(uint64_t seed, uint64_t epoch_ms) {
    ApplogIds ids;
    ids.did        = snowflake_from_seed(seed ^ 0x9E3779B97F4A7C15ULL, epoch_ms);
    ids.iid        = snowflake_from_seed(seed ^ 0xBF58476D1CE4E5B9ULL, epoch_ms);
    ids.ssid       = snowflake_from_seed(seed ^ 0x94D049BB133111EBULL, epoch_ms);
    ids.cdid       = uuid_from_seed(seed ^ 0x2545F4914F6CDD1DULL);
    ids.clientudid = uuid_from_seed(seed ^ 0x9E3779B97F4A7C15ULL);
    ids.openudid   = hex_from_seed(seed, 8);
    return ids;
}

inline bool patch_applog_xml(const std::string& real, const ApplogIds& ids,
                             std::string& out) {
    if (real.find("<map") == std::string::npos ||
        real.find("<string") == std::string::npos)
        return false;

    auto subst_contains_ci = [](const std::string& key, const char* needle) {
        size_t nl = std::strlen(needle);
        if (key.size() < nl) return false;
        for (size_t i = 0; i + nl <= key.size(); ++i) {
            size_t j = 0;
            while (j < nl) {
                char a = key[i + j];
                char b = needle[j];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                if (a != b) break;
                ++j;
            }
            if (j == nl) return true;
        }
        return false;
    };

    size_t pos = 0;
    while (pos < real.size()) {
        size_t open = real.find('<', pos);
        if (open == std::string::npos) { out.append(real, pos, std::string::npos); break; }
        out.append(real, pos, open - pos);

        size_t close = real.find('>', open);
        if (close == std::string::npos) { out.append(real, open, std::string::npos); break; }
        size_t tag_end = close + 1;

        if (real.compare(open, 7, "<string") == 0) {

            const std::string* repl = nullptr;
            size_t nam = real.find("name=\"", open);
            if (nam != std::string::npos && nam < close) {
                nam += 6;
                size_t nam_end = real.find('"', nam);
                if (nam_end != std::string::npos && nam_end < close) {
                    std::string key = real.substr(nam, nam_end - nam);
                    if      (subst_contains_ci(key, "clientudid")) repl = &ids.clientudid;
                    else if (subst_contains_ci(key, "openudid"))   repl = &ids.openudid;
                    else if (subst_contains_ci(key, "install_id")) repl = &ids.iid;
                    else if (subst_contains_ci(key, "device_id"))  repl = &ids.did;
                    else if (subst_contains_ci(key, "cdid"))       repl = &ids.cdid;
                    else if (subst_contains_ci(key, "ssid"))       repl = &ids.ssid;
                    else if (subst_contains_ci(key, "did"))        repl = &ids.did;
                    else if (subst_contains_ci(key, "iid"))        repl = &ids.iid;
                }
            }
            if (repl) {
                out.append(real, open, tag_end - open);
                out.append(*repl);
                size_t vend = real.find("</string>", tag_end);
                if (vend == std::string::npos) return false;

                size_t vtend = vend + 9;
                out.append(real, vend, vtend - vend);
                pos = vtend;
                continue;
            }
        }
        out.append(real, open, tag_end - open);
        pos = tag_end;
    }
    return true;
}

inline std::string applog_xml_synth(const ApplogIds& ids) {
    std::string x;
    x += "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n";
    x += "<map>\n";
    x += "    <string name=\"device_id\">"  + ids.did        + "</string>\n";
    x += "    <string name=\"install_id\">" + ids.iid        + "</string>\n";
    x += "    <string name=\"ssid\">"       + ids.ssid       + "</string>\n";
    x += "    <string name=\"openudid\">"   + ids.openudid   + "</string>\n";
    x += "    <string name=\"clientudid\">" + ids.clientudid + "</string>\n";
    x += "    <string name=\"cdid\">"       + ids.cdid       + "</string>\n";
    x += "</map>\n";
    return x;
}

inline int release_major(const std::string& release) {
    return std::atoi(release.c_str());
}

inline const char* kernel_base(const std::string& platform, int rmaj) {
    if (platform == "gs101" || platform == "gs201") return "5.10";
    if (platform == "zuma")                          return "5.15";
    if (platform == "zumapro" || platform == "laguna") return "6.1";
    if (rmaj <= 0)  return "5.15";
    if (rmaj <= 12) return "5.10";
    if (rmaj <= 14) return "5.15";
    return "6.1";
}

inline const char* clang_for(int rmaj) {
    if (rmaj <= 12) return "14.0.6";
    if (rmaj == 13) return "16.0.2";
    if (rmaj == 14) return "17.0.4";
    return "18.0.1";
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

    unsigned ksub = 100u + static_cast<unsigned>(seed % 120u);
    unsigned krev = 1u + static_cast<unsigned>((seed >> 8) % 15u);

    char ghash[13];
    uint64_t gh = splitmix64(seed);
    for (int i = 0; i < 12; ++i) ghash[i] = hex_lc(static_cast<unsigned>(gh >> (i * 4)));
    ghash[12] = '\0';

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

inline uint64_t ram_gb_to_memtotal_kb(int gb) {
    return static_cast<uint64_t>(gb) * 1024ULL * 1024ULL * 955ULL / 1000ULL;
}

inline uint64_t round_up_marketing_gb(uint64_t real_kb) {
    static const int tiers[] = {2, 3, 4, 6, 8, 12, 16, 18, 24};

    uint64_t gib = (real_kb + 1048575ULL) / 1048576ULL;
    for (int t : tiers) if (static_cast<uint64_t>(t) >= gib) return static_cast<uint64_t>(t);
    return gib;
}

inline std::string patch_meminfo(const std::string& real, int target_gb) {
    size_t pos = real.find("MemTotal:");
    if (pos != 0 && (pos == std::string::npos || real[pos - 1] != '\n')) {

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

inline bool patch_cpuinfo_aggregate_revision(const std::string& real,
                                             std::string& out) {
    static constexpr char prefix[] = "Processor\t: AArch64 Processor rev ";
    size_t line_end = real.find('\n');
    if (line_end == std::string::npos) line_end = real.size();
    if (line_end < sizeof(prefix) - 1 ||
        real.compare(0, sizeof(prefix) - 1, prefix) != 0)
        return false;

    size_t revision_begin = sizeof(prefix) - 1;
    size_t revision_end = revision_begin;
    while (revision_end < line_end && real[revision_end] >= '0' &&
           real[revision_end] <= '9')
        ++revision_end;
    static constexpr char suffix[] = " (aarch64)";
    if (revision_end == revision_begin ||
        line_end - revision_end != sizeof(suffix) - 1 ||
        real.compare(revision_end, sizeof(suffix) - 1, suffix) != 0)
        return false;

    size_t cursor = line_end == real.size() ? line_end : line_end + 1;
    bool have_revision = false;
    unsigned int aggregate = 0;
    while (cursor < real.size()) {
        size_t end = real.find('\n', cursor);
        if (end == std::string::npos) end = real.size();
        static constexpr char field[] = "CPU revision";
        if (end - cursor >= sizeof(field) - 1 &&
            real.compare(cursor, sizeof(field) - 1, field) == 0) {
            size_t value = cursor + sizeof(field) - 1;
            while (value < end && (real[value] == ' ' || real[value] == '\t'))
                ++value;
            if (value < end && real[value] == ':') {
                ++value;
                while (value < end && (real[value] == ' ' || real[value] == '\t'))
                    ++value;
                size_t digits = value;
                while (digits < end && real[digits] >= '0' && real[digits] <= '9')
                    ++digits;
                if (digits > value) {
                    errno = 0;
                    char* parsed_end = nullptr;
                    unsigned long parsed = std::strtoul(
                        real.substr(value, digits - value).c_str(), &parsed_end, 10);
                    if (errno == 0 && parsed_end && *parsed_end == '\0' &&
                        parsed <= 255) {
                        if (!have_revision || parsed > aggregate)
                            aggregate = static_cast<unsigned int>(parsed);
                        have_revision = true;
                    }
                }
            }
        }
        if (end == real.size()) break;
        cursor = end + 1;
    }
    if (!have_revision) return false;

    std::string replacement = std::to_string(aggregate);
    if (real.compare(revision_begin, revision_end - revision_begin,
                     replacement) == 0)
        return false;
    out = real;
    out.replace(revision_begin, revision_end - revision_begin, replacement);
    return true;
}

enum Kind {
    NONE = 0, BOOTID, MAC, VERSION, MEMINFO, CPUINFO, SELINUX_ENFORCE,

    APPLOG_XML,
    BD_RAW_DID,
    BD_RAW_IID,
    BD_RAW_OPENUDID,
    BD_RAW_CLIENTUDID,
    BD_RAW_CDID,
};

inline bool ends_with(const char* s, size_t sl, const char* suffix) {
    size_t xl = std::strlen(suffix);
    return sl >= xl && std::memcmp(s + sl - xl, suffix, xl) == 0;
}

inline bool ends_with(const std::string& s, const char* suffix) {
    return ends_with(s.c_str(), s.size(), suffix);
}

inline bool is_absolute_path(const std::string& path) {
    return !path.empty() && path[0] == '/';
}

inline bool normalize_absolute_path(const std::string& path,
                                    std::string& out) {
    if (!is_absolute_path(path) || path.size() > 4096 ||
        path.find('\0') != std::string::npos)
        return false;

    out.clear();
    out.push_back('/');
    size_t i = 1;
    while (i <= path.size()) {
        size_t slash = path.find('/', i);
        if (slash == std::string::npos) slash = path.size();
        std::string part = path.substr(i, slash - i);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                if (out.size() == 1) return false;
                size_t prev = out.find_last_of('/', out.size() - 2);
                out.erase(prev == std::string::npos ? 1 : prev + 1);
            } else {
                if (out.size() > 1 && out.back() != '/') out.push_back('/');
                out.append(part);
            }
        }
        if (slash == path.size()) break;
        i = slash + 1;
    }
    return true;
}

inline bool join_and_normalize_path(const std::string& base,
                                    const std::string& relative,
                                    std::string& out) {
    if (!is_absolute_path(base) || relative.empty() ||
        is_absolute_path(relative) || relative.size() > 4096 ||
        relative.find('\0') != std::string::npos)
        return false;
    std::string joined = base;
    if (joined.empty() || joined.back() != '/') joined.push_back('/');
    joined.append(relative);
    if (joined.size() > 4096) return false;
    return normalize_absolute_path(joined, out);
}

inline Kind classify(const char* path) {
    if (!path) return NONE;
    if (std::strcmp(path, "/proc/sys/kernel/random/boot_id") == 0) return BOOTID;
    if (std::strcmp(path, "/proc/version") == 0) return VERSION;
    if (std::strcmp(path, "/proc/meminfo") == 0) return MEMINFO;
    if (std::strcmp(path, "/proc/cpuinfo") == 0) return CPUINFO;

    if (std::strcmp(path, "/sys/fs/selinux/enforce") == 0) return SELINUX_ENFORCE;

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

    const size_t pl2 = std::strlen(path);
    if (ends_with(path, pl2, "/shared_prefs/applog.xml") ||
        ends_with(path, pl2, "/shared_prefs/snssdk_openudid.xml") ||
        ends_with(path, pl2, "/shared_prefs/snssdk_did.xml") ||
        ends_with(path, pl2, "/shared_prefs/bd_device_info.xml"))
        return APPLOG_XML;
    if (ends_with(path, pl2, "/files/bd_setting/device_id"))   return BD_RAW_DID;
    if (ends_with(path, pl2, "/files/bd_setting/install_id"))  return BD_RAW_IID;
    if (ends_with(path, pl2, "/files/bd_setting/openudid"))    return BD_RAW_OPENUDID;
    if (ends_with(path, pl2, "/files/bd_setting/clientudid"))  return BD_RAW_CLIENTUDID;
    if (ends_with(path, pl2, "/files/.cdid"))                  return BD_RAW_CDID;

    return NONE;
}

inline std::string selinux_enforce_content() { return std::string("1"); }

inline bool is_emulator_prop(const char* name) {
    if (!name) return false;

    static const char* const exact[] = {
        "ro.kernel.qemu",
        "ro.kernel.qemu.gles",
        "ro.boot.qemu",
        "ro.boot.qemu.gltransport",
        "ro.hardware.virtual_device",
        "qemu.hw.mainkeys",
        "init.svc.qemud",
        "init.svc.qemu-props",
        "init.svc.goldfish-logcat",
        "init.svc.goldfish-setup",
        "init.svc.ranchu-net",
    };
    for (const char* e : exact) if (std::strcmp(name, e) == 0) return true;

    if (std::strncmp(name, "qemu.", 5) == 0)            return true;
    if (std::strncmp(name, "ro.kernel.qemu.", 15) == 0) return true;
    if (std::strncmp(name, "ro.boot.qemu.", 13) == 0)   return true;
    return false;
}

inline bool is_custom_rom_prop(const char* name) {
    if (!name) return false;
    static const char* const exact[] = {
        "ro.modversion",
        "ro.cm.version",
        "ro.cm.build.date",
    };
    for (const char* e : exact) if (std::strcmp(name, e) == 0) return true;
    if (std::strncmp(name, "ro.lineage.", 11) == 0)          return true;
    if (std::strncmp(name, "lineage.", 8) == 0)              return true;
    if (std::strncmp(name, "ro.cm.", 6) == 0)                return true;
    if (std::strncmp(name, "persist.sys.lineage.", 20) == 0) return true;
    return false;
}

inline bool is_identity_leak_prop(const char* name) {
    if (!name) return false;
    static const char* const exact[] = {
        "ro.ril.factory_id",
        "persist.odm.ril.factory_id",
        "ro.ril.oem.imei",  "ro.ril.oem.imei0", "ro.ril.oem.imei1", "ro.ril.oem.imei2",
        "ro.ril.miui.imei", "ro.ril.miui.imei0", "ro.ril.miui.imei1", "ro.ril.miui.imei2",
        "ro.ril.oem.meid",  "ro.ril.oem.psno",  "ro.ril.oem.btmac",
        "persist.odm.ril.oem.imei0", "persist.odm.ril.oem.imei1", "persist.odm.ril.oem.imei2",
        "persist.odm.ril.oem.sno", "persist.odm.ril.oem.psno",
        "persist.odm.ril.oem.wifimac", "persist.odm.ril.oem.btmac",
        "persist.radio.imei", "persist.radio.imei0", "persist.radio.imei1", "persist.radio.imei2",
        "ro.product.serial", "ro.build.serial",
        "ro.kernel.androidboot.serialno", "ril.serialnumber",
        "gsm.sim.preiccid_0", "gsm.sim.preiccid_1",
        "persist.vendor.radio.cfu.iccid.1",
        "persist.netd.stable_secret",
    };
    for (const char* e : exact) if (std::strcmp(name, e) == 0) return true;
    return false;
}

inline bool is_oem_leak_prop(const char* name) {
    if (!name) return false;
    static const char* const exact[] = {
        "ro.product.cert",
        "ro.product.mod_device",
        "ro.fota.oem",
        "ro.netflix.bsp_rev",
        "ro.baseband",
        "persist.sys.hardcoder.name",
        "persist.vendor.sys.fp.module",
        "persist.vendor.sys.fp.vendor",
    };
    for (const char* e : exact) if (std::strcmp(name, e) == 0) return true;
    if (std::strncmp(name, "ro.miui.", 8) == 0)              return true;
    if (std::strncmp(name, "persist.sys.miui.", 17) == 0)    return true;
    if (std::strncmp(name, "ro.com.google.clientidbase", 26) == 0) return true;
    if (std::strncmp(name, "ro.vendor.miui.", 15) == 0)      return true;
    return false;
}

inline bool should_hide_prop(const char* name) {
    return is_emulator_prop(name) || is_custom_rom_prop(name) ||
           is_identity_leak_prop(name) || is_oem_leak_prop(name);
}

inline bool is_native_unsafe_prop(const char* name) {
    if (!name) return false;

    static const char* const exact[] = {
        "ro.hardware",
        "ro.product.board",
        "ro.board.platform",
        "ro.arch",
        "ro.zygote",
        "ro.vendor.api_level",
        "persist.graphics.egl",
        "ro.product.cpu.abi",
        "ro.product.cpu.abi2",
        "ro.product.cpu.abilist",
        "ro.product.cpu.abilist32",
        "ro.product.cpu.abilist64",
    };
    for (const char* e : exact) if (std::strcmp(name, e) == 0) return true;

    if (std::strncmp(name, "ro.hardware.", 12) == 0)      return true;
    if (std::strncmp(name, "ro.dalvik.vm.isa.", 17) == 0) return true;
    if (std::strncmp(name, "dalvik.vm.isa.", 14) == 0)    return true;
    return false;
}

}
