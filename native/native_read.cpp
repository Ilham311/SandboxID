#include "native_read.hpp"

namespace sbxnr {

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

uint64_t splitmix64(uint64_t& x) {
    x += 0x9E3779B97F4A7C15ULL;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void fill_bytes(uint64_t seed, uint8_t* out, size_t n) {
    uint64_t s = seed;
    size_t i = 0;
    while (i < n) {
        uint64_t v = splitmix64(s);
        for (int b = 0; b < 8 && i < n; ++b, ++i)
            out[i] = static_cast<uint8_t>(v >> (b * 8));
    }
}

std::string uuid_from_seed(uint64_t seed) {
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

std::string mac_from_seed(uint64_t seed) {
    uint8_t b[6];
    fill_bytes(seed, b, sizeof(b));
    b[0] = 0x02;
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5]);
    return std::string(buf);
}

bool is_valid_mac(const std::string& m) {
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

std::string hex_from_seed(uint64_t seed, size_t nbytes) {
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

std::string snowflake_from_seed(uint64_t seed, uint64_t epoch_ms) {
    uint64_t v = (epoch_ms << 22) | (splitmix64(seed) & 0x3FFFFFULL);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    return std::string(buf);
}

ApplogIds make_applog_ids(uint64_t seed, uint64_t epoch_ms) {
    ApplogIds ids;
    ids.did        = snowflake_from_seed(seed ^ 0x9E3779B97F4A7C15ULL, epoch_ms);
    ids.iid        = snowflake_from_seed(seed ^ 0xBF58476D1CE4E5B9ULL, epoch_ms);
    ids.ssid       = snowflake_from_seed(seed ^ 0x94D049BB133111EBULL, epoch_ms);
    ids.cdid       = uuid_from_seed(seed ^ 0x2545F4914F6CDD1DULL);
    ids.clientudid = uuid_from_seed(seed ^ 0x9E3779B97F4A7C15ULL);
    ids.openudid   = hex_from_seed(seed, 8);
    return ids;
}

bool patch_applog_xml(const std::string& real, const ApplogIds& ids,
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

std::string applog_xml_synth(const ApplogIds& ids) {
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

int release_major(const std::string& release) {
    return std::atoi(release.c_str());
}

const char* kernel_base(const std::string& platform, int rmaj) {
    if (platform == "gs101" || platform == "gs201") return "5.10";
    if (platform == "zuma")                          return "5.15";
    if (platform == "zumapro" || platform == "laguna") return "6.1";
    if (rmaj <= 0)  return "5.15";
    if (rmaj <= 12) return "5.10";
    if (rmaj <= 14) return "5.15";
    return "6.1";
}

const char* clang_for(int rmaj) {
    if (rmaj <= 12) return "14.0.6";
    if (rmaj == 13) return "16.0.2";
    if (rmaj == 14) return "17.0.4";
    return "18.0.1";
}

std::string synth_proc_version(const std::string& release,
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

int pixel_ram_gb(const std::string& model) {
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

uint64_t ram_gb_to_memtotal_kb(int gb) {
    return static_cast<uint64_t>(gb) * 1024ULL * 1024ULL * 955ULL / 1000ULL;
}

uint64_t round_up_marketing_gb(uint64_t real_kb) {
    static const int tiers[] = {2, 3, 4, 6, 8, 12, 16, 18, 24};
    uint64_t gib = (real_kb + 1048575ULL) / 1048576ULL;
    for (int t : tiers) if (static_cast<uint64_t>(t) >= gib) return static_cast<uint64_t>(t);
    return gib;
}

std::string patch_meminfo(const std::string& real, int target_gb) {
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

int cpu_action_for(const std::string& soc_manuf, const std::string& soc_model,
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
    return CPU_STRIP;
}

bool patch_cpuinfo(const std::string& real, int action,
                   const std::string& repl, std::string& out) {
    if (action == CPU_NONE) return false;
    out.clear();
    out.reserve(real.size() + 16);
    bool changed = false;
    size_t i = 0, n = real.size();
    while (i < n) {
        size_t eol = real.find('\n', i);
        size_t line_end = (eol == std::string::npos) ? n : eol;

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

// ---- Full /proc/cpuinfo synthesis for Tensor/Pixel personas ----

// Upstream arm64 /proc/cpuinfo (arch/arm64/kernel/cpuinfo.c, c_show) emits, per
// core and nothing else: processor, BogoMIPS, Features, "CPU architecture: 8",
// CPU implementer, CPU variant, CPU part, CPU revision. There is no "Hardware :"
// line and no capital-P "Processor :" line — those are the 32-bit ARM format,
// which msm-4.19-era Qualcomm vendor kernels port back over. A real Pixel's
// cpuinfo therefore starts directly at "processor : 0".
//
// Consequence for this module: under a Pixel persona, patch_cpuinfo() rewrites
// the one "Hardware :" line and leaves every per-core MIDR field untouched, so
// the served file still advertises e.g. Qualcomm 0x51 / Kryo-4xx-silver 0x805
// alongside ARM 0x41 / Cortex-A77 0xd0d — the real SoC, in the persona's own
// file. That contradiction is a direct detection tell. So for those personas we
// stop patching and rebuild the file from the persona's platform instead, the
// same way synth_proc_version() already builds a persona-matched /proc/version.
//
// Part numbers are the ARM cores each Tensor generation is actually built from
// (arch/arm64/include/asm/cputype.h), cross-checked against real Pixel dumps in
// the ThomasKaiser/sbc-bench cpuinfo archive:
//   G1 gs101   A55 0xd05 / A76 0xd0b / X1 0xd44
//   G2 gs201   A55 0xd05 / A78 0xd41 / X1 0xd44
//   G3 zuma    A510 0xd46 / A715 0xd4d / X3 0xd4e
//   G4 zumapro A520 0xd80 / A720 0xd81 / X4 0xd82
std::string cpuinfo_synth(const std::string& platform, const std::string& real,
                          uint64_t seed) {
    // The core count is taken from the real file: a persona whose cpuinfo
    // advertises a different core count than the hardware it runs on is a
    // tell of its own.
    int ncores = 0;
    {
        size_t pos = 0;
        while (pos < real.size()) {
            size_t eol = real.find('\n', pos);
            size_t le = (eol == std::string::npos) ? real.size() : eol;
            if (le - pos >= 11 && std::memcmp(real.data() + pos, "processor\t:", 11) == 0) {
                size_t j = pos + 11;
                while (j < le && real[j] == ' ') ++j;
                int n = 0;
                while (j < le && real[j] >= '0' && real[j] <= '9') {
                    n = n * 10 + (real[j] - '0');
                    ++j;
                }
                if (j > pos + 11) { ++ncores; pos = le; }
                else pos = le;
            } else {
                pos = le;
            }
            if (eol == std::string::npos) break;
            ++pos;
        }
    }
    if (ncores <= 0 || ncores > 64) return std::string();   // refuse to guess

    // mid_width is the generation's real mid-cluster size: Tensor G1/G2 are
    // 1x X* + 3x A7x + 4x A5x (8 cores), G3/G4 are 1x X* + 4x A7x + 4x A5x
    // (9 cores). A fixed width of 3 synthesises a 1+3+5 layout for the 9-core
    // parts - a per-cluster combination no real Tensor generation ships, which
    // is exactly the self-consistency tell this function exists to remove.
    struct Gen { const char* plat; const char* little; const char* mid; const char* prime; int mid_width; };
    static const Gen gens[] = {
        {"gs101",   "0xd05", "0xd0b", "0xd44", 3},
        {"gs201",   "0xd05", "0xd41", "0xd44", 3},
        {"zuma",    "0xd46", "0xd4d", "0xd4e", 4},
        {"zumapro", "0xd80", "0xd81", "0xd82", 4},
        {"laguna",  "0xd80", "0xd81", "0xd82", 4},
    };
    // Unrecognised platforms fall through to the G4 parts. That is a deliberate
    // best-effort default, not a claim: the persona pool is curated to the
    // generations above, and a hand-written persona.override naming anything
    // else gets a plausible ARM core block rather than a real one.
    const char* little = "0xd80";
    const char* mid    = "0xd81";
    const char* prime  = "0xd82";
    int mid_width = 4;
    for (const Gen& g : gens) {
        if (platform == g.plat) {
            little = g.little; mid = g.mid; prime = g.prime; mid_width = g.mid_width;
        }
    }

    // The Features and BogoMIPS lines are carried over from the real file.
    // They describe the kernel's CPU-capability surface and carry no device
    // identity, whereas inventing a list the persona's kernel could not have
    // produced would be a worse tell (Tensor kernels advertise sve/sve2/mte;
    // an msm-4.19 kernel does not).
    std::string features, bogomips;
    {
        size_t fp = real.find("Features\t:");
        if (fp != std::string::npos) {
            size_t s = fp + 10;
            size_t e = real.find('\n', s);
            if (e != std::string::npos) features = real.substr(s, e - s);
        }
        size_t bp = real.find("BogoMIPS\t:");
        if (bp == std::string::npos) bp = real.find("BogoMips\t:");
        if (bp != std::string::npos) {
            size_t s = bp + 10;
            size_t e = real.find('\n', s);
            if (e != std::string::npos) bogomips = real.substr(s, e - s);
        }
    }
    if (features.empty()) features = " fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc dcpop sha3 sm3 sm4 asimddp ssbs";
    if (bogomips.empty()) bogomips = "38.40";

    std::string out;
    out.reserve(static_cast<size_t>(ncores) * (256 + features.size()));
    for (int i = 0; i < ncores; ++i) {
        // Cluster layout: one prime core, the generation's mid-width mid cores,
        // rest little. ncores is the *real* device's count (see above), so when
        // it does not equal the generation's real core count the split is
        // inevitably a hybrid - but when it matches, this now reproduces the
        // actual silicon layout instead of a fixed 1+3+rest.
        const char* part = little;
        int cluster = 0;          // 0 = little, 1 = mid, 2 = prime
        if (i == ncores - 1) { part = prime; cluster = 2; }
        else if (i >= ncores - 1 - mid_width) { part = mid; cluster = 1; }

        // variant/revision are per-cluster and derived, so the served file is
        // deterministic for a given persona but does not copy the real SoC's
        // silicon stepping. Real MIDR variants are small (0..7), revisions 0/1.
        unsigned variant  = 1u + static_cast<unsigned>(
            (seed >> static_cast<uint64_t>(cluster * 11)) % 7u);
        unsigned revision = static_cast<unsigned>(
            (seed >> static_cast<uint64_t>(cluster * 7 + 3)) % 2u);

        char blk[512];
        std::snprintf(blk, sizeof(blk),
            "processor\t: %d\n"
            "BogoMIPS\t:%s\n"
            "Features\t:%s\n"
            "CPU implementer\t: 0x41\n"
            "CPU architecture: 8\n"
            "CPU variant\t: 0x%x\n"
            "CPU part\t: %s\n"
            "CPU revision\t: %u\n\n",
            i, bogomips.c_str(), features.c_str(), variant, part, revision);
        out.append(blk);
    }
    return out;
}

Kind classify(const char* path) {
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

bool is_emulator_prop(const char* name) {
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

bool is_custom_rom_prop(const char* name) {
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

// MIUI / HyperOS: a stock Pixel has none of these, so an in-app read should
// behave as if the property did not exist (empty), exactly like the custom-ROM
// props above. Observed on a HyperOS device: ro.miui.ui.version.name=V816,
// persist.sys.hardcoder.name=miui_booster — unambiguous OEM tells that the
// persona's Build surface contradicts.
bool is_vendor_rom_prop(const char* name) {
    if (!name) return false;
    static const char* const exact[] = {
        "persist.sys.hardcoder.name",
        "ro.com.miui.rsa",
    };
    for (const char* e : exact) if (std::strcmp(name, e) == 0) return true;
    if (std::strncmp(name, "ro.miui.", 8) == 0)          return true;
    if (std::strncmp(name, "ro.vendor.miui.", 15) == 0) return true;
    if (std::strncmp(name, "ro.ril.miui.", 12) == 0)    return true;
    if (std::strncmp(name, "persist.sys.miui.", 17) == 0) return true;
    return false;
}

bool should_hide_prop(const char* name) {
    return is_emulator_prop(name) || is_custom_rom_prop(name) ||
           is_vendor_rom_prop(name);
}

}
