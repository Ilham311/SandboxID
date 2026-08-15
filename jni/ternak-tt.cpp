
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef TT_HOST_TEST
#include <sys/mount.h>
#endif
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <random>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cctype>
#include "pool_tt.hpp"

static const char* MODDIR         = "/data/adb/modules/ternak_tt";
static const char* IDENTITY_FILE  = "/data/adb/modules/ternak_tt/identity.prop";
static const char* IDENTITY_BAK   = "/data/adb/modules/ternak_tt/identity.prop.bak";
static const char* MODE_FILE      = "/data/adb/modules/ternak_tt/identity.mode";
static const char* RESETPROP      = "/data/adb/modules/ternak_tt/bin/resetprop-rs";
static const char* MOUNTDIR       = "/data/adb/modules/ternak_tt/mount";
static const char* TARGET_FILE    = "/data/adb/modules/ternak_tt/target.txt";

static std::vector<std::string> load_targets() {
    std::vector<std::string> out;
    std::ifstream f(TARGET_FILE);
    std::string line;
    while (std::getline(f, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' ||
                line.back() == '\t' || line.back() == '\n'))
            line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        line = line.substr(s);
        if (line.empty()) continue;
        out.push_back(line);
    }
    if (out.empty()) {
        out = {
            "com.zhiliaoapp.musically",
            "com.ss.android.ugc.trill",
            "com.zhiliaoapp.musically.go",
            "com.grabtaxi.passenger",
        };
    }
    return out;
}

static std::string random_hex(int bytes, bool upper) {
    std::random_device rd;
    std::mt19937_64 gen(rd() ^ (uint64_t)std::chrono::steady_clock::now()
                              .time_since_epoch().count());
    std::uniform_int_distribution<int> d(0, 15);
    const char* al = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string s;
    s.reserve(bytes * 2);
    for (int i = 0; i < bytes * 2; ++i) s.push_back(al[d(gen)]);
    return s;
}

static bool atomic_write(const std::string& p, const std::string& data) {
    std::string tmp = p + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    ssize_t w = ::write(fd, data.data(), data.size());
    ::fsync(fd);
    ::close(fd);
    if (w != (ssize_t)data.size()) { ::unlink(tmp.c_str()); return false; }
    return ::rename(tmp.c_str(), p.c_str()) == 0;
}

static bool write_in_place(const std::string& p, const std::string& data) {
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    ssize_t w = ::write(fd, data.data(), data.size());
    ::fsync(fd);
    ::close(fd);
    return w == (ssize_t)data.size();
}

static std::string read_file(const std::string& p) {
    std::ifstream f(p);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    size_t st = s.find_first_not_of(" \t");
    if (st != std::string::npos) s = s.substr(st);
    return s;
}

// std::stoi throws on non-numeric or empty input; identity fields are
// untrusted (read from a file that may be hand-edited or corrupted), so a
// throwing parse would crash the process. Fall back to `fallback` instead.
// The module is built with -fno-exceptions (see jni/CMakeLists.txt),
// so try/catch is not allowed here. Use std::strtol instead.
static int safe_stoi(const std::string& s, int fallback) {
    if (s.empty()) return fallback;
    const char* start = s.c_str();
    char* endptr = nullptr;
    errno = 0;
    long val = std::strtol(start, &endptr, 10);

    // Check if no digits were consumed
    if (endptr == start) return fallback;

    // Check for range errors (long limits or int limits)
    if (errno == ERANGE || val < INT_MIN || val > INT_MAX) return fallback;

    // Like std::stoi, trailing non-numeric junk after the parsed number is
    // ignored rather than treated as an error.
    return static_cast<int>(val);
}

// Single source of truth for the version string: read it from module.prop at
// runtime so the CLI banner and synthetic build.prop never drift from the
// value the release pipeline stamps into module.prop.
static std::string module_version() {
    std::istringstream iss(read_file(std::string(MODDIR) + "/module.prop"));
    std::string line;
    const std::string key = "version=";
    while (std::getline(iss, line)) {
        if (line.compare(0, key.size(), key) == 0)
            return trim(line.substr(key.size()));
    }
    return "unknown";
}

static void run_bin(const char* path, std::vector<const char*> argv) {
    pid_t pid = fork();
    if (pid == 0) {
        argv.push_back(nullptr);
        execv(path, const_cast<char* const*>(argv.data()));
        _exit(127);
    } else if (pid > 0) {
        waitpid(pid, nullptr, 0);
    }
}

struct Identity {
    std::map<std::string, std::string> kv;
    std::string serialize() const {
        static const std::vector<std::string> order = {
            "BRAND","MANUFACTURER","MODEL","DEVICE","PRODUCT",
            "BOARD","HARDWARE","FINGERPRINT","ID","DISPLAY","DESCRIPTION",
            "BOOTLOADER","HOST","USER","TYPE","TAGS",
            "INCREMENTAL","RELEASE","SDK_INT","SECURITY_PATCH",
            "SERIAL","RADIO","SOC_MANUFACTURER","SOC_MODEL",
            "ANDROID_ID","GOOGLE_AID",
            "VNDK_VERSION", "BOARD_API_LEVEL", "BOARD_FIRST_API_LEVEL",
            // COPG-parity stealth persona fields (see docs/COPG-PARITY):
            // timezone + locale + carrier are applied per-app via the Zygisk
            // Java/native hooks only, never device-wide, so nothing about the
            // real device changes. FAKE_UPTIME_MS is opt-in (empty = off).
            "TIMEZONE","LOCALE","LOCALE_LANG","LOCALE_COUNTRY",
            "GSM_OPERATOR_ALPHA","GSM_OPERATOR_NUMERIC","GSM_OPERATOR_ISO",
            "FAKE_UPTIME_MS",
        };
        std::string out;
        for (const auto& k : order) {
            auto it = kv.find(k);
            if (it != kv.end()) out += k + "=" + it->second + "\n";
        }
        return out;
    }
};

static std::string uuid_v4() {
    std::string h = random_hex(16, false);
    h.insert(20, "-");
    h.insert(16, "-");
    h.insert(12, "-");
    h.insert(8, "-");
    h[14] = '4';
    static const char* v = "89ab";
    std::random_device rd;
    std::mt19937 g(rd());
    h[19] = v[g() % 4];
    return h;
}

static Identity gen_identity() {
    std::random_device rd;
    std::mt19937 g(rd());
    constexpr size_t N = sizeof(TT_POOL) / sizeof(TT_POOL[0]);
    const PixelEntry& p = TT_POOL[g() % N];

    Identity id;
    id.kv["BRAND"]           = "google";
    id.kv["MANUFACTURER"]    = "Google";
    id.kv["MODEL"]           = p.model;
    id.kv["DEVICE"]          = p.device;
    id.kv["PRODUCT"]         = p.product;
    id.kv["BOARD"]           = p.board;
    id.kv["HARDWARE"]        = p.board;
    id.kv["ID"]              = p.id;
    id.kv["INCREMENTAL"]     = p.incremental;
    id.kv["RELEASE"]         = p.release;
    id.kv["SDK_INT"]         = std::to_string(p.sdk);
    id.kv["SECURITY_PATCH"]  = p.security_patch;
    // BOOTLOADER is overwritten below with a device-consistent value; the "unknown"
    // literal that used to live here was written into ro.bootloader and stood out
    // in every fingerprint (no shipping Pixel reports that string).
    id.kv["HOST"]            = "abfarm-release";
    id.kv["USER"]            = "android-build";
    id.kv["TYPE"]            = "user";
    id.kv["TAGS"]            = "release-keys";

    char fp[512];
    snprintf(fp, sizeof(fp), "google/%s/%s:%s/%s/%s:user/release-keys",
             p.product, p.device, p.release, p.id, p.incremental);
    id.kv["FINGERPRINT"] = fp;
    id.kv["DISPLAY"]     = p.id;

    char desc[512];
    snprintf(desc, sizeof(desc), "%s-user %s %s %s release-keys",
             p.product, p.release, p.id, p.incremental);
    id.kv["DESCRIPTION"] = desc;

    // --- RADIO / baseband ----------------------------------------------------
    // A real Pixel modem string looks like:
    //     g5300q-241015-241028-B-12244875
    //                ^d1     ^d2
    // where d1 is the RIL compile date and d2 is the OTA sign-off date (a few
    // days later, never identical). Setting both to the same value was a
    // deterministic fingerprint of the previous rotator, and hardcoding the
    // "g5300q" prefix regardless of SoC generation made "Pixel 6 reporting a
    // G3-era modem" a trivial anti-fraud tell.
    //
    // We now:
    //   1. Anchor the two dates a few days apart, both in UTC (timegm) so the
    //      generator behaves the same regardless of the box's local TZ.
    //   2. Pick the chipset-appropriate prefix from PixelEntry.baseband_prefix.
    std::string sp = p.security_patch; // YYYY-MM-DD
    int sp_year = 2024, sp_mon = 1, sp_day = 1;
    if (sp.size() >= 10) {
        sp_year = safe_stoi(sp.substr(0, 4), sp_year);
        sp_mon  = safe_stoi(sp.substr(5, 2), sp_mon);
        sp_day  = safe_stoi(sp.substr(8, 2), sp_day);
    }
    struct tm sp_tm = {0};
    sp_tm.tm_year = sp_year - 1900;
    sp_tm.tm_mon  = sp_mon - 1;
    sp_tm.tm_mday = sp_day;
    // timegm interprets the tm as UTC. mktime uses the process TZ, which would
    // shift the generated date by a day on a device that's set to UTC-negative
    // and whose SECURITY_PATCH is early in the month.
    std::time_t sp_time = timegm(&sp_tm);

    // OTA sign-off (d2) sits between 30 and 120 days before the security patch
    // ships. RIL compile (d1) is 1-14 days earlier than the OTA sign-off. This
    // keeps the two dates monotonically ordered and never equal.
    std::uniform_int_distribution<> back_dist(30, 120);
    std::uniform_int_distribution<> gap_dist(1, 14);
    std::time_t d2_time = sp_time - (back_dist(g) * 86400LL);
    std::time_t d1_time = d2_time - (gap_dist(g)  * 86400LL);
    struct tm d1_tm{}, d2_tm{};
    gmtime_r(&d1_time, &d1_tm);
    gmtime_r(&d2_time, &d2_tm);

    char d1_str[16], d2_str[16];
    strftime(d1_str, sizeof(d1_str), "%y%m%d", &d1_tm);
    strftime(d2_str, sizeof(d2_str), "%y%m%d", &d2_tm);
    char rad[128];
    snprintf(rad, sizeof(rad), "%s-%s-%s-B-%s",
             p.baseband_prefix, d1_str, d2_str, p.incremental);
    id.kv["RADIO"] = rad;

    // BOOTLOADER: real Pixels report "<device>-<board-rev>-<incremental>". The
    // previous "unknown" value stood out because no shipping Pixel ever writes
    // that literal string to ro.bootloader in a release build.
    char bl[128];
    snprintf(bl, sizeof(bl), "%s-1.0-%s", p.device, p.incremental);
    id.kv["BOOTLOADER"] = bl;

    id.kv["SERIAL"]     = random_hex(8, true);
    id.kv["ANDROID_ID"] = random_hex(8, false);
    id.kv["GOOGLE_AID"] = uuid_v4();

    // --- COPG-parity stealth identity fields --------------------------------
    // SoC is well-known per Pixel generation, so we can spoof it truthfully
    // (a Pixel 8 must report Tensor G3). Wrong values would be a detection
    // vector, so we only set what we know.
    id.kv["SOC_MANUFACTURER"] = "Google";
    id.kv["SOC_MODEL"]        = p.soc;

    // Region persona: default to the ID market (Asia/Jakarta + id-ID) so the
    // timezone, locale and SIM carrier all agree. Users can override any of
    // these at runtime from the WebUI ("Region" tab -> ternak-tt set).
    id.kv["TIMEZONE"]       = "Asia/Jakarta";
    id.kv["LOCALE"]         = "id-ID";
    id.kv["LOCALE_LANG"]    = "id";
    id.kv["LOCALE_COUNTRY"] = "ID";

    constexpr size_t NC = sizeof(TT_CARRIERS) / sizeof(TT_CARRIERS[0]);
    const CarrierEntry& c = TT_CARRIERS[g() % NC];
    id.kv["GSM_OPERATOR_ALPHA"]   = c.name;
    id.kv["GSM_OPERATOR_NUMERIC"] = c.mccmnc;
    id.kv["GSM_OPERATOR_ISO"]     = c.iso;

    // Derive VNDK and API level from RELEASE
    std::string rel_str = p.release;
    if (rel_str == "13") {
        id.kv["VNDK_VERSION"] = "33";
        id.kv["BOARD_FIRST_API_LEVEL"] = "33";
        id.kv["BOARD_API_LEVEL"] = "202305";
    } else if (rel_str == "14") {
        id.kv["VNDK_VERSION"] = "34";
        id.kv["BOARD_FIRST_API_LEVEL"] = "34";
        id.kv["BOARD_API_LEVEL"] = "202404";
    } else if (rel_str == "15") {
        id.kv["VNDK_VERSION"] = "35";
        id.kv["BOARD_FIRST_API_LEVEL"] = "35";
        id.kv["BOARD_API_LEVEL"] = "202404";
    } else if (rel_str == "16") {
        id.kv["VNDK_VERSION"] = "36";
        id.kv["BOARD_FIRST_API_LEVEL"] = "36";
        id.kv["BOARD_API_LEVEL"] = "202504";
    } else {
        fprintf(stderr, "unsupported RELEASE=%s, extend the VNDK derivation table\n", p.release);
        exit(2);
    }

    // FAKE_UPTIME_MS intentionally left unset: fake uptime is opt-in and off
    // for a fresh persona. The WebUI can enable it (ternak-tt set FAKE_UPTIME_MS).
    return id;
}

#ifdef TT_DEBUG
#define TT_VARIANT_TAG "debug"
#define DBG(fmt, ...) fprintf(stderr, "[D] " fmt "\n", ##__VA_ARGS__)
#else
#define TT_VARIANT_TAG "release"
#define DBG(...) ((void)0)
#endif

#ifndef TT_HOST_TEST
// PERF: apply_native forks resetprop-rs serially (~60 calls, 2-4s on slow devices).
// Batching with bounded background & + wait was considered (issue #21) but DEFERRED:
// - Requires real-device benchmarking (>=200ms improvement threshold) not available in CI.
// - Serial ordering avoids inter-property race conditions.
// Revisit when a device timing harness is available. Do NOT batch speculatively.
static void apply_native(const Identity& id) {
    DBG("apply_native: enter (identity has %zu kv pairs)", id.kv.size());
    auto get = [&](const char* k) -> std::string {
        auto it = id.kv.find(k);
        return it != id.kv.end() ? it->second : std::string();
    };

    struct Rp { const char* key; std::string val; };

    const std::string SERIAL       = get("SERIAL");
    const std::string MODEL        = get("MODEL");
    const std::string BRAND        = get("BRAND");
    const std::string MANUFACTURER = get("MANUFACTURER");
    const std::string DEVICE       = get("DEVICE");
    const std::string PRODUCT      = get("PRODUCT");
    const std::string BOARD        = get("BOARD");
    const std::string ID_          = get("ID");
    const std::string FP           = get("FINGERPRINT");
    const std::string DISPLAY      = get("DISPLAY");
    const std::string DESC         = get("DESCRIPTION");
    const std::string RELEASE      = get("RELEASE");
    const std::string SDK_INT      = get("SDK_INT");
    const std::string SECPATCH     = get("SECURITY_PATCH");
    const std::string INCREMENTAL  = get("INCREMENTAL");
    const std::string BOOTLOADER   = get("BOOTLOADER");
    const std::string RADIO        = get("RADIO");
    const std::string TAGS         = get("TAGS");
    const std::string TYPE         = get("TYPE");
    const std::string USER_        = get("USER");
    const std::string HOST         = get("HOST");
    const std::string SOC_MFR      = get("SOC_MANUFACTURER");
    const std::string SOC_MODEL    = get("SOC_MODEL");

    std::vector<Rp> rp = {

        {"ro.serialno",                        SERIAL},
        {"ro.boot.serialno",                   SERIAL},

        {"ro.build.fingerprint",               FP},
        {"ro.bootimage.build.fingerprint",     FP},
        {"ro.system.build.fingerprint",        FP},
        {"ro.vendor.build.fingerprint",        FP},
        {"ro.odm.build.fingerprint",           FP},
        {"ro.product.build.fingerprint",       FP},
        {"ro.system_ext.build.fingerprint",    FP},

        {"ro.product.model",                   MODEL},
        {"ro.product.system.model",            MODEL},
        {"ro.product.vendor.model",            MODEL},
        {"ro.product.odm.model",               MODEL},
        {"ro.product.product.model",           MODEL},
        {"ro.product.system_ext.model",        MODEL},

        {"ro.product.brand",                   BRAND},
        {"ro.product.system.brand",            BRAND},
        {"ro.product.vendor.brand",            BRAND},
        {"ro.product.odm.brand",               BRAND},
        {"ro.product.product.brand",           BRAND},
        {"ro.product.system_ext.brand",        BRAND},

        {"ro.product.manufacturer",            MANUFACTURER},
        {"ro.product.system.manufacturer",     MANUFACTURER},
        {"ro.product.vendor.manufacturer",     MANUFACTURER},
        {"ro.product.odm.manufacturer",        MANUFACTURER},
        {"ro.product.product.manufacturer",    MANUFACTURER},
        {"ro.product.system_ext.manufacturer", MANUFACTURER},

        {"ro.product.device",                  DEVICE},
        {"ro.product.system.device",           DEVICE},
        {"ro.product.vendor.device",           DEVICE},
        {"ro.product.odm.device",              DEVICE},
        {"ro.product.product.device",          DEVICE},
        {"ro.product.system_ext.device",       DEVICE},

        {"ro.product.name",                    PRODUCT},
        {"ro.product.system.name",             PRODUCT},
        {"ro.product.vendor.name",             PRODUCT},
        {"ro.product.odm.name",                PRODUCT},
        {"ro.product.product.name",            PRODUCT},
        {"ro.product.system_ext.name",         PRODUCT},

        {"ro.product.board",                   BOARD},
        {"ro.build.product",                   DEVICE},

        {"ro.build.id",                        ID_},
        {"ro.build.display.id",                DISPLAY},
        {"ro.build.description",               DESC},
        {"ro.build.tags",                      TAGS},
        {"ro.build.type",                      TYPE},
        {"ro.build.user",                      USER_},
        {"ro.build.host",                      HOST},

        {"ro.build.version.release",           RELEASE},
        {"ro.build.version.release_or_codename", RELEASE},
        {"ro.build.version.sdk",               SDK_INT},
        {"ro.system.build.version.sdk",        SDK_INT},
        {"ro.vendor.build.version.sdk",        SDK_INT},
        {"ro.build.version.security_patch",    SECPATCH},
        {"ro.vendor.build.security_patch",     SECPATCH},
        {"ro.build.version.incremental",       INCREMENTAL},

        {"gsm.version.baseband",               RADIO},
        {"ro.build.expect.baseband",           RADIO},

        {"ro.soc.manufacturer",                SOC_MFR},
        {"ro.soc.model",                       SOC_MODEL},

        {"ro.bootloader",                      BOOTLOADER},
        {"ro.boot.bootloader",                 BOOTLOADER},
    };

    if (::access(RESETPROP, X_OK) == 0) {
        int applied = 0;
        for (const auto& r : rp) {
            if (r.val.empty()) continue;
            run_bin(RESETPROP, {"resetprop-rs", "-n", r.key, r.val.c_str()});
            applied++;
        }
        printf("  Native prop: %d set via resetprop-rs\n", applied);
    } else {
        fprintf(stderr, "! resetprop-rs missing at %s (native prop skipped)\n", RESETPROP);
    }

    std::string aid = get("ANDROID_ID");
    if (!aid.empty()) {
        run_bin("/system/bin/settings",
                {"settings", "put", "secure", "android_id", aid.c_str()});
    }
    if (!MODEL.empty()) {
        run_bin("/system/bin/settings",
                {"settings", "put", "global", "device_name", MODEL.c_str()});
    }
}
#endif

static void generate_mount_files(const Identity& id) {
    DBG("generate_mount_files: MOUNTDIR=%s", MOUNTDIR);
    auto g = [&](const char* k) -> std::string {
        auto it = id.kv.find(k);
        return it != id.kv.end() ? it->second : std::string();
    };

    ::mkdir(MOUNTDIR, 0755);
    for (const char* sub : {"system", "vendor", "odm", "product", "system_ext"}) {
        std::string d = std::string(MOUNTDIR) + "/" + sub;
        ::mkdir(d.c_str(), 0755);
    }

    const std::string SERIAL       = g("SERIAL");
    const std::string MODEL        = g("MODEL");
    const std::string BRAND        = g("BRAND");
    const std::string MANUFACTURER = g("MANUFACTURER");
    const std::string DEVICE       = g("DEVICE");
    const std::string PRODUCT      = g("PRODUCT");
    const std::string BOARD        = g("BOARD");
    const std::string BOOTLOADER   = g("BOOTLOADER");
    const std::string ID_          = g("ID");
    const std::string FP           = g("FINGERPRINT");
    const std::string DISPLAY      = g("DISPLAY");
    const std::string DESC         = g("DESCRIPTION");
    const std::string RELEASE      = g("RELEASE");
    const std::string SDK          = g("SDK_INT");
    const std::string SECPATCH     = g("SECURITY_PATCH");
    const std::string INCREMENTAL  = g("INCREMENTAL");
    const std::string RADIO        = g("RADIO");
    const std::string TAGS         = g("TAGS");
    const std::string TYPE         = g("TYPE");
    const std::string USER_        = g("USER");
    const std::string HOST         = g("HOST");
    const std::string SOC_MFR      = g("SOC_MANUFACTURER");
    const std::string SOC_MODEL    = g("SOC_MODEL");
    const std::string LOCALE       = g("LOCALE");
    const std::string LOCALE_LANG  = g("LOCALE_LANG");
    const std::string LOCALE_CC    = g("LOCALE_COUNTRY");

    std::string base;
    base += "# Ternak TT synthetic build.prop (" + module_version() + ")\n";
    auto add = [&](const char* k, const std::string& v) {
        if (!v.empty()) { base += k; base += '='; base += v; base += '\n'; }
    };
    add("ro.serialno",                        SERIAL);
    add("ro.boot.serialno",                   SERIAL);
    add("ro.build.fingerprint",               FP);
    add("ro.bootimage.build.fingerprint",     FP);
    add("ro.system.build.fingerprint",        FP);
    add("ro.vendor.build.fingerprint",        FP);
    add("ro.odm.build.fingerprint",           FP);
    add("ro.product.build.fingerprint",       FP);
    add("ro.system_ext.build.fingerprint",    FP);
    add("ro.product.model",                   MODEL);
    add("ro.product.brand",                   BRAND);
    add("ro.product.manufacturer",            MANUFACTURER);
    add("ro.product.device",                  DEVICE);
    add("ro.product.name",                    PRODUCT);
    add("ro.product.board",                   BOARD);
    add("ro.build.id",                        ID_);
    add("ro.build.display.id",                DISPLAY);
    add("ro.build.description",               DESC);
    add("ro.build.tags",                      TAGS);
    add("ro.build.type",                      TYPE);
    add("ro.build.user",                      USER_);
    add("ro.build.host",                      HOST);
    add("ro.build.version.release",           RELEASE);
    add("ro.build.version.release_or_codename", RELEASE);
    add("ro.build.version.sdk",               SDK);
    add("ro.build.version.security_patch",    SECPATCH);
    add("ro.build.version.incremental",       INCREMENTAL);
    add("ro.bootloader",                      BOOTLOADER);
    add("ro.boot.bootloader",                 BOOTLOADER);
    add("ro.build.product",                   DEVICE);
    add("gsm.version.baseband",               RADIO);
    add("ro.build.expect.baseband",           RADIO);
    add("ro.soc.manufacturer",                SOC_MFR);
    add("ro.soc.model",                       SOC_MODEL);
    // Locale lives in the app-scoped build.prop overlay only (bind-mounted into
    // the target's mount namespace), so anti-fraud SDKs that parse build.prop
    // read the persona locale while the real device UI language is untouched.
    add("ro.product.locale",                  LOCALE);
    add("ro.product.locale.language",         LOCALE_LANG);
    add("ro.product.locale.region",           LOCALE_CC);

    struct { const char* dir; const char* pfx; } parts[] = {
        {"system",     "ro.product.system."},
        {"vendor",     "ro.product.vendor."},
        {"odm",        "ro.product.odm."},
        {"product",    "ro.product.product."},
        {"system_ext", "ro.product.system_ext."},
    };
    for (const auto& p : parts) {
        std::string c = base;
        std::string pfx = p.pfx;
        c += "# Partition alias (" + std::string(p.dir) + ")\n";
        if (!MODEL.empty())        c += pfx + "model="        + MODEL        + "\n";
        if (!BRAND.empty())        c += pfx + "brand="        + BRAND        + "\n";
        if (!MANUFACTURER.empty()) c += pfx + "manufacturer=" + MANUFACTURER + "\n";
        if (!DEVICE.empty())       c += pfx + "device="       + DEVICE       + "\n";
        if (!PRODUCT.empty())      c += pfx + "name="         + PRODUCT      + "\n";
        std::string path = std::string(MOUNTDIR) + "/" + p.dir + "/build.prop";
        if (!write_in_place(path, c)) {
            fprintf(stderr, "! failed to write %s (partial overlay, persona may be inconsistent)\n", path.c_str());
        }
        ::chmod(path.c_str(), 0644);
    }

    std::string aid  = g("ANDROID_ID");
    std::string gaid = g("GOOGLE_AID");
    if (aid.empty()) aid = "0000000000000000";

    std::string xml;
    xml += "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n";
    xml += "<settings version=\"217\">\n";
    xml += "  <setting id=\"1\" name=\"android_id\" value=\"" + aid
         + "\" package=\"android\" defaultValue=\"" + aid
         + "\" defaultSysSet=\"true\" />\n";
    if (!gaid.empty()) {
        xml += "  <setting id=\"2\" name=\"advertising_id\" value=\"" + gaid
             + "\" package=\"com.google.android.gms\" />\n";
        xml += "  <setting id=\"3\" name=\"limit_ad_tracking\" value=\"0\" "
               "package=\"com.google.android.gms\" />\n";
    }
    xml += "</settings>\n";

    std::string xml_path = std::string(MOUNTDIR) + "/settings_secure.xml";
    if (!write_in_place(xml_path, xml)) {
        fprintf(stderr, "! failed to write %s (partial file, android_id/GAID overlay may be stale)\n", xml_path.c_str());
    }

    ::chmod(xml_path.c_str(), 0600);
    ::chown(xml_path.c_str(), 1000, 1000);

#ifndef TT_HOST_TEST
    run_bin("/system/bin/chcon", {"chcon", "u:object_r:system_file:s0",
            (std::string(MOUNTDIR) + "/system/build.prop").c_str()});
    run_bin("/system/bin/chcon", {"chcon", "u:object_r:vendor_file:s0",
            (std::string(MOUNTDIR) + "/vendor/build.prop").c_str()});
    for (const char* sub : {"odm", "product", "system_ext"}) {
        std::string p = std::string(MOUNTDIR) + "/" + sub + "/build.prop";
        run_bin("/system/bin/chcon",
                {"chcon", "u:object_r:system_file:s0", p.c_str()});
    }
    run_bin("/system/bin/chcon", {"chcon", "u:object_r:system_data_file:s0",
            xml_path.c_str()});
#endif
    printf("  Mount overlay: 5 build.prop + settings_secure.xml -> %s\n", MOUNTDIR);
}

static void wipe_tt_data() {
#ifndef TT_HOST_TEST
    auto pkgs = load_targets();
    for (const auto& pkg : pkgs) {
        run_bin("/system/bin/pm", {"pm", "clear", pkg.c_str()});
        run_bin("/system/bin/am", {"am", "force-stop", pkg.c_str()});
    }
#endif
}

static int cmd_targets() {
    auto pkgs = load_targets();
    struct stat st{};
    bool have_file = (::stat(TARGET_FILE, &st) == 0);
    printf("target.txt : %s%s\n",
           TARGET_FILE,
           have_file ? "" : "  (missing — using built-in defaults)");
    printf("count      : %zu\n\n", pkgs.size());
    for (const auto& p : pkgs) printf("  %s\n", p.c_str());
    return 0;
}

static Identity load_identity_from_file(const std::string& path) {
    Identity id;
    std::istringstream iss(read_file(path.c_str()));
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        id.kv[line.substr(0, eq)] = trim(line.substr(eq + 1));
    }
    return id;
}

static Identity load_identity_from_string(const std::string& data) {
    Identity id;
    std::istringstream iss(data);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        id.kv[line.substr(0, eq)] = trim(line.substr(eq + 1));
    }
    return id;
}

static Identity load_identity() {
    return load_identity_from_file(IDENTITY_FILE);
}

// Persona Consistency Validation logic
static bool validate_identity(const Identity& id, std::vector<std::string>& errors) {
    auto get = [&](const char* k) -> std::string {
        auto it = id.kv.find(k);
        return it != id.kv.end() ? it->second : std::string();
    };

    bool valid = true;
    auto fail = [&](const std::string& msg) {
        errors.push_back(msg);
        valid = false;
    };

    const std::string soc_mfr = get("SOC_MANUFACTURER");
    const std::string soc_model = get("SOC_MODEL");
    const std::string radio = get("RADIO");
    const std::string inc = get("INCREMENTAL");

    const std::string sec_patch = get("SECURITY_PATCH");
    int sec_year = 9999;
    if (sec_patch.size() >= 4) sec_year = safe_stoi(sec_patch.substr(0, 4), sec_year);

    if (soc_mfr == "Google" && soc_model.compare(0, 6, "Tensor") == 0) {
        // Every Tensor-era baseband is "<prefix>-YYMMDD-YYMMDD-B-<INCREMENTAL>".
        // The prefix is chipset-scoped, not "g5300q for everyone" — Pixel 6/6a
        // uses g5123b, Pixel 7 series uses g5300b, Pixel 8/9 uses g5300q, and
        // Pixel 10 uses g5300s. Reject any RADIO that doesn't match the
        // chipset-appropriate family and doesn't have INCREMENTAL as its suffix.
        //
        // For SoC families we don't have hard data on, we fall back to the
        // shared "starts with g5" check to stay permissive rather than block a
        // fresh Pixel entry that the pool hasn't been taught about yet.
        const char* want_prefix = nullptr;
        if      (soc_model == "Tensor")     want_prefix = "g5123b";
        else if (soc_model == "Tensor G2")  want_prefix = "g5300b";
        else if (soc_model == "Tensor G3")  want_prefix = "g5300q";
        else if (soc_model == "Tensor G4")  want_prefix = "g5300q";
        else if (soc_model == "Tensor G5")  want_prefix = "g5300s";

        bool prefix_ok = want_prefix
            ? radio.compare(0, std::strlen(want_prefix), want_prefix) == 0
            : radio.compare(0, 2, "g5") == 0;

        if (!prefix_ok || radio.find("-B-" + inc) == std::string::npos) {
            fail("RADIO mismatch: " + soc_model + " expects prefix "
                 + (want_prefix ? want_prefix : "g5") + " and -B-<INCREMENTAL="
                 + inc + "> suffix, got " + radio);
        } else if (radio.size() >= 20) {
            // Format is "<prefix>-YYMMDD-YYMMDD-...". Locate the two dates by
            // scanning from the tail so a longer prefix (e.g. "g5300b") does
            // not shift the fixed-offset positions the old parser assumed.
            //
            // We index from just after the leading prefix + "-", parse d1 and
            // d2, and require d1 <= d2. Real Pixel modem strings never carry
            // "start > end" and never carry "start == end" either — the OTA
            // sign-off happens 1-14 days after RIL compile.
            const size_t pfx_len = want_prefix
                ? std::strlen(want_prefix)
                : radio.find('-');
            if (pfx_len != std::string::npos && radio.size() >= pfx_len + 14
                && radio[pfx_len] == '-' && radio[pfx_len + 7] == '-') {
                std::string d1_str = radio.substr(pfx_len + 1, 6);
                std::string d2_str = radio.substr(pfx_len + 8, 6);

                // Convert YYMMDD to a comparable integer (YYYYMMDD in 20xx).
                auto parse6 = [&](const std::string& s) -> long {
                    if (s.size() != 6) return -1;
                    for (char c : s) if (c < '0' || c > '9') return -1;
                    return 200000L * 100 + std::strtol(s.c_str(), nullptr, 10);
                };
                long d1 = parse6(d1_str);
                long d2 = parse6(d2_str);
                if (d1 < 0 || d2 < 0) {
                    fail("RADIO date parse: " + d1_str + " / " + d2_str);
                } else if (d1 > d2) {
                    fail("RADIO dates out of order: d1=" + d1_str
                         + " > d2=" + d2_str);
                }

                int y1 = 2000 + safe_stoi(d1_str.substr(0, 2), 0);
                int y2 = 2000 + safe_stoi(d2_str.substr(0, 2), 0);
                if (y1 > sec_year || y2 > sec_year) {
                    fail("radio_date_future: RADIO date (" + d1_str + " / "
                         + d2_str + ") > SECURITY_PATCH year ("
                         + std::to_string(sec_year) + ")");
                }
            } else {
                fail("RADIO layout: could not locate two YYMMDD dates in " + radio);
            }
        }
    }

    const std::string release = get("RELEASE");
    const std::string vndk = get("VNDK_VERSION");
    const std::string api_level = get("BOARD_API_LEVEL");
    const std::string first_api_level = get("BOARD_FIRST_API_LEVEL");

    std::string exp_vndk, exp_api_level, exp_first_api_level;
    if (release == "13") {
        exp_vndk = "33"; exp_api_level = "202305"; exp_first_api_level = "33";
    } else if (release == "14") {
        exp_vndk = "34"; exp_api_level = "202404"; exp_first_api_level = "34";
    } else if (release == "15") {
        exp_vndk = "35"; exp_api_level = "202404"; exp_first_api_level = "35";
    } else if (release == "16") {
        exp_vndk = "36"; exp_api_level = "202504"; exp_first_api_level = "36";
    }

    if (!exp_vndk.empty()) {
        if (vndk != exp_vndk || api_level != exp_api_level || first_api_level != exp_first_api_level) {
            fail("vndk_release_mismatch: RELEASE=" + release + " requires VNDK=" + exp_vndk +
                 ", BOARD_API_LEVEL=" + exp_api_level + "; got vndk=" + vndk + ", api_level=" + api_level);
        }
    }

    const std::string locale = get("LOCALE");
    const std::string loc_lang = get("LOCALE_LANG");
    const std::string loc_country = get("LOCALE_COUNTRY");

    if (!locale.empty() && (!loc_lang.empty() || !loc_country.empty())) {
        if (locale != loc_lang + "-" + loc_country) {
            fail("LOCALE format mismatch: " + locale + " != " + loc_lang + "-" + loc_country);
        }
    }

    if (!sec_patch.empty()) {
        // sec_patch format is YYYY-MM-DD
        if (sec_patch.size() == 10 && sec_patch[4] == '-' && sec_patch[7] == '-') {
            std::time_t now = std::time(nullptr);
            struct tm lt;
            localtime_r(&now, &lt);
            char date[16];
            strftime(date, sizeof(date), "%Y-%m-%d", &lt);
            if (sec_patch > std::string(date)) {
                fail("SECURITY_PATCH is in the future: " + sec_patch + " > " + date);
            }
        } else {
            fail("SECURITY_PATCH invalid format: " + sec_patch);
        }
    }

    return valid;
}

static int cmd_validate(int argc, char** argv) {
    std::string path = IDENTITY_FILE;
    if (argc >= 3) {
        path = argv[2];
    }

    Identity id;
    if (path == "-") {
        std::string data;
        char buf[4096];
        while (true) {
            ssize_t got = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (got <= 0) break;
            data.append(buf, got);
        }
        id = load_identity_from_string(data);
    } else {
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) {
            fprintf(stderr, "FAIL: I/O: failed to read %s\n", path.c_str());
            return 1;
        }
        id = load_identity_from_file(path);
    }

    if (id.kv.empty()) {
        fprintf(stderr, "FAIL: Parse: identity is empty or malformed\n");
        return 1;
    }

    std::vector<std::string> errors;
    bool valid = validate_identity(id, errors);

    if (valid) {
        printf("OK: RADIO pattern\n");
        printf("OK: LOCALE format\n");
        printf("OK: SECURITY_PATCH date\n");
        printf("validate: %zu passed, 0 failed\n", (size_t)3);
        return 0;
    } else {
        for (const auto& err : errors) {
            fprintf(stderr, "FAIL: %s\n", err.c_str());
        }
        printf("validate: 0 passed, %zu failed\n", errors.size());
        return 2;
    }
}

static bool ensure_root() {
    if (geteuid() != 0) {
        fprintf(stderr, "! ternak-tt must run as root. Use: su -c ternak-tt <cmd>\n");
        return false;
    }
    return true;
}

// Rotate the pool `iters` times, running validate_identity on each generated
// persona. This is the regression net for gen_identity: every future edit to
// pool_tt.hpp or the RADIO / VNDK derivation must keep this loop green.
// Runs in both host and target builds so CI can wire it into tests/run_tests.sh.
static int cmd_selfcheck(int argc, char** argv) {
    int iters = 200;
    if (argc >= 3) {
        int v = safe_stoi(argv[2], -1);
        if (v > 0) iters = v;
    }
    int failed = 0;
    for (int i = 0; i < iters; ++i) {
        Identity id = gen_identity();
        std::vector<std::string> errors;
        if (!validate_identity(id, errors)) {
            failed++;
            fprintf(stderr, "iter %d FAILED (MODEL=%s SOC=%s RADIO=%s):\n",
                    i,
                    id.kv["MODEL"].c_str(),
                    id.kv["SOC_MODEL"].c_str(),
                    id.kv["RADIO"].c_str());
            for (const auto& e : errors) fprintf(stderr, "  - %s\n", e.c_str());
        }
    }
    printf("selfcheck: %d iterations, %d failed\n", iters, failed);
    return failed == 0 ? 0 : 2;
}

static int cmd_freshen() {
    DBG("cmd_freshen: build=%s", TT_VARIANT_TAG);
    if (!ensure_root()) return 1;

    std::string mode = trim(read_file(MODE_FILE));
    if (mode == "locked") {
        printf("LOCKED: run `ternak-tt unlock` first\n");
        return 1;
    }

    std::string old = read_file(IDENTITY_FILE);

    Identity id = gen_identity();

    std::vector<std::string> errors;
    if (!validate_identity(id, errors)) {
        for (const auto& err : errors) {
            fprintf(stderr, "freshen: validation failed: %s\n", err.c_str());
        }
        return 2;
    }

    if (!old.empty()) atomic_write(IDENTITY_BAK, old);

    if (!atomic_write(IDENTITY_FILE, id.serialize())) {
        fprintf(stderr, "! failed to write identity.prop\n");
        return 1;
    }

#ifndef TT_HOST_TEST
    apply_native(id);
#endif
    generate_mount_files(id);
    wipe_tt_data();

    printf("OK - fresh TT persona ready\n");
    printf("  MODEL       : %s\n", id.kv["MODEL"].c_str());
    printf("  DEVICE      : %s\n", id.kv["DEVICE"].c_str());
    printf("  RELEASE     : %s (SDK %s)\n",
           id.kv["RELEASE"].c_str(), id.kv["SDK_INT"].c_str());
    printf("  FINGERPRINT : %s\n", id.kv["FINGERPRINT"].c_str());
    printf("  SERIAL      : %s\n", id.kv["SERIAL"].c_str());
    printf("  ANDROID_ID  : %s\n", id.kv["ANDROID_ID"].c_str());
    printf("  GAID        : %s\n", id.kv["GOOGLE_AID"].c_str());
    printf("  SOC         : %s %s\n",
           id.kv["SOC_MANUFACTURER"].c_str(), id.kv["SOC_MODEL"].c_str());
    printf("  TIMEZONE    : %s\n", id.kv["TIMEZONE"].c_str());
    printf("  LOCALE      : %s\n", id.kv["LOCALE"].c_str());
    printf("  CARRIER     : %s (%s / %s)\n",
           id.kv["GSM_OPERATOR_ALPHA"].c_str(),
           id.kv["GSM_OPERATOR_NUMERIC"].c_str(),
           id.kv["GSM_OPERATOR_ISO"].c_str());
    printf("  SEC PATCH   : %s\n", id.kv["SECURITY_PATCH"].c_str());

    auto pkgs = load_targets();
    printf("  Wiped: %zu pkg(s) from target.txt\n", pkgs.size());
    for (const auto& p : pkgs) printf("    - %s\n", p.c_str());
    return 0;
}

static int cmd_status() {
    std::string d = read_file(IDENTITY_FILE);
    if (d.empty()) {
        printf("no identity yet - run `ternak-tt freshen`\n");
        return 0;
    }
    fputs(d.c_str(), stdout);
    return 0;
}

#ifndef TT_HOST_TEST
static int cmd_apply_boot_impl();
static int cmd_apply_boot() { return cmd_apply_boot_impl(); }
static int cmd_apply_boot_impl() {
    if (!ensure_root()) return 1;
    Identity id = load_identity();
    if (id.kv.empty()) {
        printf("no identity yet\n");
        return 0;
    }
    apply_native(id);
    generate_mount_files(id);
    printf("OK: native prop re-applied + mount overlay refreshed\n");
    return 0;
}
#endif

#ifndef TT_HOST_TEST
static int cmd_mount_overlay() {
    struct BindEntry { const char* src_rel; const char* dst; };
    static const BindEntry BIND_ENTRIES[] = {
        {"system/build.prop",     "/system/build.prop"},
        {"vendor/build.prop",     "/vendor/build.prop"},
        {"odm/build.prop",        "/odm/etc/build.prop"},
        {"odm/build.prop",        "/odm/build.prop"},
        {"product/build.prop",    "/product/etc/build.prop"},
        {"product/build.prop",    "/product/build.prop"},
        {"system_ext/build.prop", "/system_ext/etc/build.prop"},
        {"system_ext/build.prop", "/system_ext/build.prop"},
        {"settings_secure.xml",   "/data/system/users/0/settings_secure.xml"},
    };

    int ok_count = 0;
    for (const auto& e : BIND_ENTRIES) {
        std::string src = std::string(MOUNTDIR) + "/" + e.src_rel;

        // 1. Verify src is readable
        if (::access(src.c_str(), R_OK) != 0) continue;

        // 2. Verify dst is a regular file
        struct stat st;
        if (::lstat(e.dst, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        // Bind mount does NOT require write access to the underlying filesystem
        // — the kernel just overlays the source dentry on top of the target,
        // and the destination fs stays as ro as it was. The previous code
        // tried to "MS_REMOUNT|MS_BIND" the parent on EACCES/EROFS, which is
        // both wrong (bind can't be blocked by fs ro) and dangerous (it drops
        // the ro flag on /system, /vendor, etc. across the *global* mount
        // namespace we're standing in at seed time).
        //
        // If mount(2) fails here, the two realistic causes are:
        //   * EPERM/EINVAL  — kernel mount-lock; must be seeded pre-zygote via
        //                     post-fs-data.sh, no userspace remedy.
        //   * EACCES        — SELinux; needs a policy exception, not a remount.
        // We report the errno and move on; overlay stealth is preserved.
        if (::mount(src.c_str(), e.dst, nullptr, MS_BIND, nullptr) == 0) {
            // Detach this new mount from the shared/master group so subsequent
            // rotations don't propagate through peers. Best-effort: kernels
            // that reject MS_PRIVATE on a fresh bind fall back to shared, which
            // is the pre-fix behaviour.
            ::mount(nullptr, e.dst, nullptr, MS_PRIVATE, nullptr);
            ok_count++;
            continue;
        }

        fprintf(stderr, "! mount %s -> %s failed: errno=%d (%s)\n",
                src.c_str(), e.dst, errno, strerror(errno));
    }
    return ok_count;
}
#endif

static int cmd_seed() {
    if (!ensure_root()) return 1;
    Identity id;
    std::string existing = read_file(IDENTITY_FILE);
    if (!existing.empty()) {
        id = load_identity();
        DBG("seed: reusing existing identity (%zu keys)", id.kv.size());
    } else {
        DBG("seed: no identity yet, generating fresh");
        id = gen_identity();
        if (!atomic_write(IDENTITY_FILE, id.serialize())) {
            fprintf(stderr, "! seed: failed to write %s\n", IDENTITY_FILE);
            return 1;
        }
    }
    generate_mount_files(id);
#ifndef TT_HOST_TEST
    int mount_rc = cmd_mount_overlay();
    printf("OK: seed complete (overlay mounted: %d/9)\n", mount_rc);
#else
    printf("OK: seed complete (mount overlay ready at %s)\n", MOUNTDIR);
#endif
    return 0;
}

static int cmd_lock() {
    if (!ensure_root()) return 1;
    atomic_write(MODE_FILE, "locked\n");
    printf("OK: locked\n");
    return 0;
}

static int cmd_unlock() {
    if (!ensure_root()) return 1;
    atomic_write(MODE_FILE, "fresh\n");
    printf("OK: unlocked\n");
    return 0;
}

// Runtime-editable persona fields exposed to the WebUI "Region" tab. Only the
// stealth, per-app-scoped fields are settable here; the core device profile
// (model/fingerprint/serial) is owned by `freshen` and must not be poked
// piecemeal, or the persona would drift out of internal consistency.
static bool is_settable_key(const std::string& k) {
    static const char* allow[] = {
        "TIMEZONE", "LOCALE", "LOCALE_LANG", "LOCALE_COUNTRY",
        "GSM_OPERATOR_ALPHA", "GSM_OPERATOR_NUMERIC", "GSM_OPERATOR_ISO",
        "FAKE_UPTIME_MS",
    };
    for (const char* a : allow) if (k == a) return true;
    return false;
}

// Line-based key upsert that preserves every other line (including shell-owned
// keys like WIFI_MAC / BLUETOOTH_ADDR that rotate_ids.sh appends). We do NOT go
// through Identity::serialize here because that would drop any key outside its
// fixed order list.
static int cmd_set(const std::string& key, const std::string& value) {
    if (!ensure_root()) return 1;
    if (key.empty() || !is_settable_key(key)) {
        fprintf(stderr, "! set: key '%s' is not runtime-settable\n", key.c_str());
        return 1;
    }
    // Values are single-line prop entries; a newline would corrupt the file.
    if (value.find('\n') != std::string::npos || value.find('\r') != std::string::npos) {
        fprintf(stderr, "! set: value must not contain newlines\n");
        return 1;
    }
    if (key == "FAKE_UPTIME_MS" && !value.empty()) {
        for (char ch : value) {
            if (ch < '0' || ch > '9') {
                fprintf(stderr, "! set: FAKE_UPTIME_MS must be a non-negative integer (ms)\n");
                return 1;
            }
        }
    }

    // LOCALE, LOCALE_LANG and LOCALE_COUNTRY must stay consistent with the
    // "LANG-COUNTRY" == LOCALE invariant enforced by validate_identity(),
    // so setting any one of them also recomputes the other two.
    std::map<std::string, std::string> updates;
    if (key == "LOCALE") {
        auto dash = value.find('-');
        if (dash == std::string::npos) {
            fprintf(stderr, "! set: LOCALE must be in lang-COUNTRY form (e.g. en-US)\n");
            return 1;
        }
        updates["LOCALE"]         = value;
        updates["LOCALE_LANG"]    = value.substr(0, dash);
        updates["LOCALE_COUNTRY"] = value.substr(dash + 1);
    } else if (key == "LOCALE_LANG" || key == "LOCALE_COUNTRY") {
        Identity cur = load_identity_from_file(IDENTITY_FILE);
        std::string lang    = key == "LOCALE_LANG"    ? value : cur.kv["LOCALE_LANG"];
        std::string country = key == "LOCALE_COUNTRY" ? value : cur.kv["LOCALE_COUNTRY"];
        updates["LOCALE_LANG"]    = lang;
        updates["LOCALE_COUNTRY"] = country;
        updates["LOCALE"]         = lang + "-" + country;
    } else {
        updates[key] = value;
    }

    std::istringstream iss(read_file(IDENTITY_FILE));
    std::string line, out;
    std::map<std::string, bool> replaced;
    for (const auto& u : updates) replaced[u.first] = false;
    while (std::getline(iss, line)) {
        std::string probe = line;
        if (!probe.empty() && probe.back() == '\r') probe.pop_back();
        auto eq = probe.find('=');
        std::string k = eq != std::string::npos ? probe.substr(0, eq) : std::string();
        auto it = updates.find(k);
        if (it != updates.end()) {
            if (!replaced[k]) { out += k + "=" + it->second + "\n"; replaced[k] = true; }
            continue;  // drop old / duplicate lines for this key
        }
        out += probe + "\n";
    }
    for (const auto& u : updates) {
        if (!replaced[u.first]) out += u.first + "=" + u.second + "\n";
    }

    if (!atomic_write(IDENTITY_FILE, out)) {
        fprintf(stderr, "! set: failed to write %s\n", IDENTITY_FILE);
        return 1;
    }
    ::chmod(IDENTITY_FILE, 0644);
    printf("OK: %s=%s\n", key.c_str(), value.c_str());
    printf("  (reopen the target app to apply; run `ternak-tt apply-boot` to sync props)\n");
    return 0;
}

static int cmd_rollback() {
    if (!ensure_root()) return 1;
    std::string d = read_file(IDENTITY_BAK);
    if (d.empty()) {
        printf("no backup\n");
        return 1;
    }
    atomic_write(IDENTITY_FILE, d);
    Identity rid = load_identity();
#ifndef TT_HOST_TEST
    apply_native(rid);
#endif
    generate_mount_files(rid);
    wipe_tt_data();
    printf("OK: rolled back + wiped\n");
    return 0;
}

static void usage(const char* p) {
    fprintf(stderr,
        "Ternak TT %s - TikTok Zygisk fresh persona (standalone)\n\n"
        "Usage: %s <command>\n\n"
        "  freshen      Rotate identity + wipe TT app data (main action)\n"
        "  status       Print current identity.prop\n"
        "  rollback     Restore previous identity from backup\n"
        "  lock         Prevent freshen (safety)\n"
        "  unlock       Re-enable freshen\n"
        "  apply-boot   Re-apply native prop (used by service.sh)\n"
        "  seed         Fast bootstrap: identity + mount overlay only\n"
        "               (used by post-fs-data.sh, no native/wipe)\n"
        "  set K V      Set a runtime persona field (TIMEZONE, LOCALE,\n"
        "               LOCALE_LANG, LOCALE_COUNTRY, GSM_OPERATOR_ALPHA,\n"
        "               GSM_OPERATOR_NUMERIC, GSM_OPERATOR_ISO, FAKE_UPTIME_MS)\n"
        "  targets      List current target packages from target.txt\n"
        "  validate     Validate an identity.prop file for consistency\n"
        "  selfcheck [N] Regenerate + validate the persona N times (default 200)\n",
        module_version().c_str(), p);
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const char* c = argv[1];
    if (!strcmp(c, "freshen"))    return cmd_freshen();
    if (!strcmp(c, "status"))     return cmd_status();
    if (!strcmp(c, "rollback"))   return cmd_rollback();
    if (!strcmp(c, "lock"))       return cmd_lock();
    if (!strcmp(c, "unlock"))     return cmd_unlock();
#ifndef TT_HOST_TEST
    if (!strcmp(c, "apply-boot")) return cmd_apply_boot();
#endif
    if (!strcmp(c, "seed"))       return cmd_seed();
    if (!strcmp(c, "validate"))   return cmd_validate(argc, argv);
    if (!strcmp(c, "set")) {
        if (argc < 3) { fprintf(stderr, "! set: usage: ternak-tt set <KEY> <VALUE>\n"); return 1; }
        return cmd_set(argv[2], argc >= 4 ? argv[3] : "");
    }
    if (!strcmp(c, "targets"))    return cmd_targets();
    if (!strcmp(c, "selfcheck"))  return cmd_selfcheck(argc, argv);
    usage(argv[0]);
    return 1;
}
