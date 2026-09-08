
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
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
#include "config.hpp"
#include "sbx_carrier.hpp"
#include "sbx_identity.hpp"
#include "sbx_native_read.hpp"
#include "sbx_property.hpp"
#include <sys/system_properties.h>

static const char* IDENTITY_FILE  = sandboxid::IDENTITY_FILE;
static const char* IDENTITY_BAK   = sandboxid::IDENTITY_BAK;
static const char* MODE_FILE      = sandboxid::MODE_FILE;
static const char* MOUNTDIR       = sandboxid::MOUNTDIR;
static const char* TARGET_FILE    = sandboxid::TARGET_FILE;
static const char* PERSONAS_FILE  = sandboxid::PERSONAS_FILE;
static const char* PERSONA_OVERRIDE = sandboxid::PERSONA_OVERRIDE;
static const char* CARRIER_CONF   = sandboxid::CARRIER_CONF;
static const char* LEGACY_SETTINGS_OVERLAY = sandboxid::LEGACY_SETTINGS_OVERLAY;

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
    std::string tmp = p + ".tmp." + std::to_string((long)::getpid());
    ::unlink(tmp.c_str());

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = true;
    for (size_t off = 0; off < data.size(); ) {
        ssize_t w = ::write(fd, data.data() + off, data.size() - off);
        if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
        off += (size_t)w;
    }
    ::fsync(fd);
    ::close(fd);
    if (!ok) { ::unlink(tmp.c_str()); return false; }
    if (::rename(tmp.c_str(), p.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
    size_t slash = p.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? std::string(".")
                      : (slash == 0 ? std::string("/") : p.substr(0, slash));
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
    return true;
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

static int run_bin(const char* path, std::vector<const char*> argv, bool null_io = false) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (null_io) {
            int nul = ::open("/dev/null", O_RDWR | O_CLOEXEC);
            if (nul >= 0) {
                dup2(nul, STDIN_FILENO);
                dup2(nul, STDOUT_FILENO);
                dup2(nul, STDERR_FILENO);
                if (nul > STDERR_FILENO) ::close(nul);
            }
        }
        argv.push_back(nullptr);
        execv(path, const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

struct Identity {
    std::map<std::string, std::string> kv;

    std::string serialize() const {
        static constexpr std::string_view order[] = {
        "BRAND","MANUFACTURER","MODEL","MARKETNAME","DEVICE","PRODUCT",
        "BOARD","HARDWARE","BOARD_PLATFORM","SOC_MANUFACTURER","SOC_MODEL",
        "FINGERPRINT","ID","DISPLAY","DESCRIPTION",
        "BOOTLOADER","HOST","USER","TYPE","TAGS",
        "INCREMENTAL","RELEASE","SDK_INT","SECURITY_PATCH",
        "SERIAL","RADIO","ANDROID_ID","GOOGLE_AID",
        "GSM_OPERATOR_NUMERIC","GSM_OPERATOR_ALPHA","GSM_OPERATOR_ISO","GSM_SIM_STATE",
        "SKU","ODM_SKU","BUILD_TIME_UTC","BUILD_DATE","FLAVOR","APPLOG_EPOCH",
        "SBX_NATIVE_READ","SBX_HIDE","SBX_CPU_REVISION",
        "SBX_PROC_VERSION","SBX_MEMINFO","SBX_SYSFS_MAC",
        };
        std::string out;
        if (!sbxid::serialize_identity_values(
                kv, order, sizeof(order) / sizeof(order[0]), out))
            return {};
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

static int device_sdk() {
    char b[PROP_VALUE_MAX] = {0};
    if (__system_property_get("ro.build.version.sdk", b) > 0) return atoi(b);
    return 0;
}

static bool runtime_is_stable_release() {
    char preview_sdk[PROP_VALUE_MAX] = {0};
    char codename[PROP_VALUE_MAX] = {0};
    if (__system_property_get("ro.build.version.preview_sdk", preview_sdk) <= 0 ||
        __system_property_get("ro.build.version.codename", codename) <= 0)
        return false;
    return sbxprop::stable_release_runtime(preview_sdk, codename);
}

static std::string gen_host_suffix() {
    std::random_device rd;
    std::mt19937 g(rd());
    static const char* prefixes[] = {
        "abfarm", "abfarm-release", "abfarm-server", "build", "build-server",
        "release", "release-server", "farm", "buildfarm",
    };
    constexpr int n_prefixes = sizeof(prefixes) / sizeof(prefixes[0]);
    std::string host = prefixes[g() % n_prefixes];

    host += "-";
    host += std::to_string(g() % 900 + 100);
    return host;
}

struct PixelEntry {
    std::string model, device, product, board, platform;
    int         sdk = 0;
    std::string release, id, incremental, security_patch;

    std::string brand, manufacturer, marketname, soc_manufacturer, soc_model, radio;
};

static bool is_tensor_platform(const std::string& plat) {
    return plat == "gs101" || plat == "gs201" || plat == "zuma" ||
           plat == "zumapro" || plat == "laguna";
}

static long long build_utc_from_patch(const std::string& patch,
                                      const std::string& incremental) {
    if (patch.size() < 10 || patch[4] != '-' || patch[7] != '-') return 0;
    const int y = atoi(patch.substr(0, 4).c_str());
    const int m = atoi(patch.substr(5, 2).c_str());
    const int d = atoi(patch.substr(8, 2).c_str());
    if (y < 2008 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) return 0;

    int digits = 0;
    for (char c : incremental) if (c >= '0' && c <= '9') digits += c - '0';

    struct tm tmv{};
    tmv.tm_year = y - 1900;
    tmv.tm_mon  = m - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = 3;
    time_t t = timegm(&tmv);
    if (t == (time_t)-1) return 0;

    t -= (time_t)(1 + digits % 6) * 86400;
    t += (time_t)(digits % 60) * 60 + (time_t)(digits % 37);
    return (long long)t;
}

static std::string build_date_string(long long utc) {
    if (utc <= 0) return "";
    time_t t = (time_t)utc;
    struct tm tmv{};
    if (!gmtime_r(&t, &tmv)) return "";
    char buf[64];
    if (strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S UTC %Y", &tmv) == 0) return "";
    return buf;
}

static std::vector<PixelEntry> builtin_personas() {
    std::vector<PixelEntry> v;

    auto add = [&](const char* mo, const char* de, const char* pl, int sd,
                   const char* re, const char* id, const char* in, const char* sp) {
        PixelEntry e; e.model = mo; e.device = de; e.product = de; e.board = de;
        e.platform = pl; e.sdk = sd; e.release = re; e.id = id;
        e.incremental = in; e.security_patch = sp; v.push_back(std::move(e));
    };
    add("Pixel 6",  "oriole",  "gs101",  31, "12", "SD1A.210817.036", "7805805",  "2021-10-05");
    add("Pixel 6",  "oriole",  "gs101",  33, "13", "TQ3A.230901.001", "10750268", "2023-09-05");
    add("Pixel 8",  "shiba",   "zuma",   35, "15", "AP3A.240905.015", "12244875", "2024-09-05");
    add("Pixel 10", "frankel", "laguna", 36, "16", "BP1A.250705.006", "13051207", "2025-07-05");
    return v;
}

static bool parse_persona_line(const std::string& raw, PixelEntry& out) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    if (line.empty() || line[0] == '#') return false;

    std::vector<std::string> col;
    std::string cur;
    std::istringstream ss(line);
    while (std::getline(ss, cur, '\t')) col.push_back(cur);
    if (col.size() < 10) return false;

    out.model          = col[0];
    out.device         = col[1];
    out.product        = col[2];
    out.board          = col[3];
    out.platform       = col[4];
    out.sdk            = atoi(col[5].c_str());
    out.release        = col[6];
    out.id             = col[7];
    out.incremental    = col[8];
    out.security_patch = col[9];

    if (col.size() > 10) out.brand            = col[10];
    if (col.size() > 11) out.manufacturer     = col[11];
    if (col.size() > 12) out.marketname       = col[12];
    if (col.size() > 13) out.soc_manufacturer = col[13];
    if (col.size() > 14) out.soc_model        = col[14];
    if (col.size() > 15) out.radio            = col[15];

    if (out.device.empty() || out.sdk <= 0) return false;

    if (!is_tensor_platform(out.platform)) {
        if (out.brand.empty() || out.soc_model.empty()) {
            fprintf(stderr,
                    "! persona '%s' has non-Tensor platform '%s' but is missing "
                    "brand/soc_model columns — skipping\n",
                    out.device.c_str(), out.platform.c_str());
            return false;
        }
    }
    return true;
}

static std::vector<PixelEntry> load_personas() {
    std::vector<PixelEntry> pool;
    std::ifstream f(PERSONAS_FILE);
    std::string line;
    while (std::getline(f, line)) {
        PixelEntry e;
        if (parse_persona_line(line, e)) pool.push_back(std::move(e));
    }
    if (pool.empty()) {
        fprintf(stderr,
                "! persona pool empty/unreadable (%s) — using built-in fallback\n",
                PERSONAS_FILE);
        pool = builtin_personas();
    }
    return pool;
}

static bool pick_persona(PixelEntry& out, std::string& error) {
    std::random_device rd;
    std::mt19937 g(rd());

    std::vector<PixelEntry> pool = load_personas();
    const int dev = device_sdk();
    if (dev <= 0) {
        error = "device SDK is unavailable";
        return false;
    }

    std::vector<size_t> candidates;
    for (size_t i = 0; i < pool.size(); ++i)
        if (pool[i].sdk == dev) candidates.push_back(i);
    if (candidates.empty()) {
        error = "no exact persona for runtime SDK " + std::to_string(dev);
        return false;
    }

    out = pool[candidates[g() % candidates.size()]];
    error.clear();
    return true;
}

static Identity derive_identity(const PixelEntry& p) {
    const bool tensor = is_tensor_platform(p.platform);

    auto tensor_soc_model = [](const std::string& plat) -> const char* {
        if (plat == "gs101")   return "GS101";
        if (plat == "gs201")   return "GS201";
        if (plat == "zuma")    return "GS301";
        if (plat == "zumapro") return "GS401";
        if (plat == "laguna")  return "GS501";
        return "unknown";
    };
    auto tensor_modem_prefix = [](const std::string& plat) -> const char* {
        if (plat == "gs101")   return "g5123b";
        if (plat == "gs201")   return "g5300b";
        if (plat == "zuma")    return "g5300q";
        if (plat == "zumapro") return "g5400";
        if (plat == "laguna")  return "g5500";
        return "unknown";
    };

    const std::string brand   = p.brand.empty()        ? "google" : p.brand;
    const std::string manuf   = p.manufacturer.empty() ? "Google" : p.manufacturer;
    const std::string soc_man = !p.soc_manufacturer.empty() ? p.soc_manufacturer
                                : (tensor ? "Google" : std::string());
    const std::string soc_mod = !p.soc_model.empty() ? p.soc_model
                                : (tensor ? tensor_soc_model(p.platform) : std::string());

    Identity id;
    id.kv["BRAND"]           = brand;
    id.kv["MANUFACTURER"]    = manuf;
    id.kv["MODEL"]           = p.model;

    id.kv["MARKETNAME"]      = p.marketname.empty() ? p.model : p.marketname;
    id.kv["DEVICE"]          = p.device;
    id.kv["PRODUCT"]         = p.product;
    id.kv["BOARD"]           = p.board;
    id.kv["HARDWARE"]        = p.board;
    id.kv["BOARD_PLATFORM"]  = p.platform;
    id.kv["SOC_MANUFACTURER"] = soc_man;
    id.kv["SOC_MODEL"]       = soc_mod;
    id.kv["ID"]              = p.id;
    id.kv["INCREMENTAL"]     = p.incremental;
    id.kv["RELEASE"]         = p.release;
    id.kv["SDK_INT"]         = std::to_string(p.sdk);
    id.kv["SECURITY_PATCH"]  = p.security_patch;
    id.kv["BOOTLOADER"]      = "unknown";
    id.kv["HOST"]            = gen_host_suffix();
    id.kv["USER"]            = "android-build";
    id.kv["TYPE"]            = "user";
    id.kv["TAGS"]            = "release-keys";

    id.kv["FLAVOR"]          = p.product + "-" + id.kv["TYPE"];

    char fp[512];
    snprintf(fp, sizeof(fp), "%s/%s/%s:%s/%s/%s:user/release-keys",
             brand.c_str(), p.product.c_str(), p.device.c_str(),
             p.release.c_str(), p.id.c_str(), p.incremental.c_str());
    id.kv["FINGERPRINT"] = fp;
    id.kv["DISPLAY"]     = p.id;

    char desc[512];
    snprintf(desc, sizeof(desc), "%s-user %s %s %s release-keys",
             p.product.c_str(), p.release.c_str(), p.id.c_str(), p.incremental.c_str());
    id.kv["DESCRIPTION"] = desc;

    if (!p.radio.empty()) {
        id.kv["RADIO"] = p.radio;
    } else if (tensor) {
        char pdate[8] = "000000";
        if (p.security_patch.size() >= 10) {
            pdate[0] = p.security_patch[2]; pdate[1] = p.security_patch[3];
            pdate[2] = p.security_patch[5]; pdate[3] = p.security_patch[6];
            pdate[4] = p.security_patch[8]; pdate[5] = p.security_patch[9];
        }
        char rad[128];
        snprintf(rad, sizeof(rad), "%s-%s-B-%s",
                 tensor_modem_prefix(p.platform), pdate, p.incremental.c_str());
        id.kv["RADIO"] = rad;
    }

    id.kv["SERIAL"]     = random_hex(8, true);
    id.kv["ANDROID_ID"] = random_hex(8, false);
    id.kv["GOOGLE_AID"] = uuid_v4();

    {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
        if (now_ms <= 0) now_ms = 1700000000000LL;
        id.kv["APPLOG_EPOCH"] = std::to_string(now_ms);
    }

    {
        long long utc = build_utc_from_patch(p.security_patch, p.incremental);
        if (utc > 0) {
            id.kv["BUILD_TIME_UTC"] = std::to_string(utc);
            id.kv["BUILD_DATE"]     = build_date_string(utc);
        }
    }

    id.kv["SKU"]     = "";
    id.kv["ODM_SKU"] = "";
    id.kv["SBX_NATIVE_READ"] = "1";
    id.kv["SBX_HIDE"] = "0";
    id.kv["SBX_CPU_REVISION"] = "0";
    id.kv["SBX_PROC_VERSION"] = "0";
    id.kv["SBX_MEMINFO"] = "0";
    id.kv["SBX_SYSFS_MAC"] = "0";

    return id;
}

static bool gen_identity(Identity& out, std::string& error) {
    PixelEntry persona;
    if (!pick_persona(persona, error)) return false;
    out = derive_identity(persona);
    return true;
}

static bool take_persona_override(PixelEntry& out) {
    std::string raw = read_file(PERSONA_OVERRIDE);
    if (raw.empty()) return false;
    bool ok = false;
    std::istringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        if (parse_persona_line(line, out)) { ok = true; break; }
    }
    ::unlink(PERSONA_OVERRIDE);
    return ok;
}

#ifdef SBX_DEBUG
#define SBX_VARIANT_TAG "debug"
#define DBG(fmt, ...) fprintf(stderr, "[D] " fmt "\n", ##__VA_ARGS__)
#else
#define SBX_VARIANT_TAG "release"
#define DBG(...) ((void)0)
#endif

static void generate_mount_files(const Identity& id) {
    DBG("generate_mount_files: MOUNTDIR=%s", MOUNTDIR);
    auto g = [&](const char* k) -> std::string {
        auto it = id.kv.find(k);
        return it != id.kv.end() ? it->second : std::string();
    };

    ::mkdir(MOUNTDIR, 0755);
    for (size_t i = 0; i < sandboxid::MOUNT_PARTS_N; ++i) {
        std::string d = std::string(MOUNTDIR) + "/" + sandboxid::MOUNT_PARTS[i];
        ::mkdir(d.c_str(), 0755);
    }

    const std::string SERIAL       = g("SERIAL");
    const std::string MODEL        = g("MODEL");
    const std::string BRAND        = g("BRAND");
    const std::string MANUFACTURER = g("MANUFACTURER");
    const std::string DEVICE       = g("DEVICE");
    const std::string PRODUCT      = g("PRODUCT");
    const std::string ID_          = g("ID");
    const std::string FP           = g("FINGERPRINT");
    const std::string DISPLAY      = g("DISPLAY");
    const std::string DESC         = g("DESCRIPTION");
    const std::string RELEASE      = g("RELEASE");
    const std::string SECPATCH     = g("SECURITY_PATCH");
    const std::string INCREMENTAL  = g("INCREMENTAL");
    const std::string RADIO        = g("RADIO");
    const std::string TAGS         = g("TAGS");
    const std::string TYPE         = g("TYPE");
    const std::string USER_        = g("USER");
    const std::string HOST         = g("HOST");
    const std::string MARKETNAME   = g("MARKETNAME");
    const bool stable_release      = runtime_is_stable_release();

    std::string base;
    base += "# begin build properties\n";
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
    add("ro.vendor_dlkm.build.fingerprint",   FP);
    add("ro.odm_dlkm.build.fingerprint",      FP);
    add("ro.product.model",                   MODEL);
    add("ro.product.brand",                   BRAND);
    add("ro.product.manufacturer",            MANUFACTURER);
    add("ro.product.device",                  DEVICE);
    add("ro.product.name",                    PRODUCT);
    add("ro.product.marketname",              MARKETNAME);
    add("ro.product.vendor.marketname",       MARKETNAME);
    add("ro.product.odm.marketname",          MARKETNAME);
    add("ro.product.system.marketname",       MARKETNAME);
    add("ro.product.product.marketname",      MARKETNAME);
    add("ro.build.id",                        ID_);
    add("ro.build.display.id",                DISPLAY);
    add("ro.build.description",               DESC);
    add("ro.build.tags",                      TAGS);
    add("ro.build.type",                      TYPE);
    add("ro.build.user",                      USER_);
    add("ro.build.host",                      HOST);
    add("ro.build.flavor",                    g("FLAVOR"));
    add("ro.build.date.utc",                  g("BUILD_TIME_UTC"));
    add("ro.build.date",                      g("BUILD_DATE"));
    add("ro.build.version.release",           RELEASE);
    if (stable_release)
        add("ro.build.version.release_or_codename", RELEASE);
    add("ro.build.version.security_patch",    SECPATCH);
    add("ro.build.version.incremental",       INCREMENTAL);

    add("ro.boot.hardware.sku",               g("SKU"));
    add("ro.boot.product.hardware.sku",       g("ODM_SKU"));

    add("ro.bootloader",                      std::string("unknown"));
    add("ro.boot.bootloader",                 std::string("unknown"));
    add("ro.build.product",                   DEVICE);
    add("gsm.version.baseband",               RADIO);
    add("ro.build.expect.baseband",           RADIO);

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
        if (!MODEL.empty())        c += pfx + "model="        + MODEL        + "\n";
        if (!BRAND.empty())        c += pfx + "brand="        + BRAND        + "\n";
        if (!MANUFACTURER.empty()) c += pfx + "manufacturer=" + MANUFACTURER + "\n";
        if (!DEVICE.empty())       c += pfx + "device="       + DEVICE       + "\n";
        if (!PRODUCT.empty())      c += pfx + "name="         + PRODUCT      + "\n";
        if (!MARKETNAME.empty())   c += pfx + "marketname="   + MARKETNAME   + "\n";
        std::string ppfx = std::string("ro.") + p.dir + ".build.";
        c += ppfx + "id=" + ID_ + "\n";
        c += ppfx + "fingerprint=" + FP + "\n";
        c += ppfx + "type=" + TYPE + "\n";
        c += ppfx + "tags=" + TAGS + "\n";
        c += ppfx + "version.incremental=" + INCREMENTAL + "\n";
        c += ppfx + "version.release=" + RELEASE + "\n";
        if (stable_release)
            c += ppfx + "version.release_or_codename=" + RELEASE + "\n";
        if (!g("BUILD_TIME_UTC").empty()) {
            c += ppfx + "date.utc=" + g("BUILD_TIME_UTC") + "\n";
            c += ppfx + "date=" + g("BUILD_DATE") + "\n";
        }
        if (std::string(p.dir) == "vendor") {
            c += "ro.vendor.build.security_patch=" + SECPATCH + "\n";
        }
        std::string path = std::string(MOUNTDIR) + "/" + p.dir + "/build.prop";
        atomic_write(path, c);
        ::chmod(path.c_str(), 0644);
    }

    struct { const char* sub; const char* ctx; } part_ctx[] = {
        {"system",     "u:object_r:system_file:s0"},
        {"vendor",     "u:object_r:vendor_file:s0"},
        {"odm",        "u:object_r:vendor_file:s0"},
        {"product",    "u:object_r:system_file:s0"},
        {"system_ext", "u:object_r:system_file:s0"},
    };
    for (const auto& pc : part_ctx) {
        std::string p = std::string(MOUNTDIR) + "/" + pc.sub + "/build.prop";
        run_bin("/system/bin/chcon", {"chcon", pc.ctx, p.c_str()});
    }

    ::unlink(LEGACY_SETTINGS_OVERLAY);
    printf("  Mount overlay: 5 build.prop trees -> %s\n", MOUNTDIR);
}

static int cmd_targets() {
    auto pkgs = load_targets();
    struct stat st{};
    bool have_file = (::stat(TARGET_FILE, &st) == 0);

    printf("target.txt : %s%s\n",
           TARGET_FILE,
           have_file ? "" : "  (missing — no targets configured; module idle)");
    printf("count      : %zu\n\n", pkgs.size());
    for (const auto& p : pkgs) printf("  %s\n", p.c_str());
    return 0;
}

static bool load_identity_file(const char* path, Identity& id, std::string& error,
                               bool* needs_migration = nullptr) {
    if (needs_migration) *needs_migration = false;
    const std::string blob = read_file(path);
    if (blob.empty()) {
        error = std::string(path) + " is empty or unreadable";
        return false;
    }
    const int sdk = device_sdk();
    if (sdk <= 0) {
        error = "device SDK is unavailable";
        return false;
    }
    sbxid::IdentitySnapshot snapshot;
    sbxid::ValidationContext context;
    context.runtime_sdk = sdk;
    context.max_blob = sandboxid::MAX_IDENTITY_BLOB;
    context.drop_legacy_capabilities = true;
    if (!sbxid::parse_and_validate_identity(blob, context, snapshot, error))
        return false;
    if (!snapshot.dropped_legacy_capabilities.empty()) {
        fprintf(stderr, "* ignored %zu legacy runtime-capability key(s) in %s\n",
                snapshot.dropped_legacy_capabilities.size(), path);
        if (needs_migration) *needs_migration = true;
    }
    id.kv = std::move(snapshot.values);
    return true;
}

static bool validate_identity(const Identity& id, std::string& error) {
    const int sdk = device_sdk();
    if (sdk <= 0) {
        error = "device SDK is unavailable";
        return false;
    }
    sbxid::IdentitySnapshot snapshot;
    sbxid::ValidationContext context;
    context.runtime_sdk = sdk;
    context.max_blob = sandboxid::MAX_IDENTITY_BLOB;
    return sbxid::parse_and_validate_identity(id.serialize(), context, snapshot, error);
}

static bool merge_carrier(Identity& id) {

    sbxcarrier::CarrierSel sel = sbxcarrier::parse_carrier_conf(read_file(CARRIER_CONF));
    return sbxcarrier::apply_carrier(id.kv, sel);
}

static bool ensure_root() {
    if (geteuid() != 0) {
        fprintf(stderr, "! sandboxid must run as root. Use: su -c sandboxid <cmd>\n");
        return false;
    }
    return true;
}

static int cmd_freshen() {
    DBG("cmd_freshen: build=%s", SBX_VARIANT_TAG);
    if (!ensure_root()) return 1;

    std::string mode = trim(read_file(MODE_FILE));
    if (mode == "locked") {
        printf("LOCKED: run `sandboxid unlock` first\n");
        return 1;
    }

    std::string old = read_file(IDENTITY_FILE);

    PixelEntry ov;
    Identity id;
    std::string identity_error;
    const int dev_sdk = device_sdk();
    if (take_persona_override(ov)) {
        if (dev_sdk <= 0 || ov.sdk != dev_sdk) {
            fprintf(stderr,
                    "! autopif persona %s uses SDK %d but runtime SDK is %d; "
                    "exact SDK match is required\n",
                    ov.model.c_str(), ov.sdk, dev_sdk);
            return 1;
        }
        fprintf(stderr, "* autopif persona: %s (%s/%s, SDK %d) — exact runtime match\n",
                ov.model.c_str(), ov.device.c_str(), ov.platform.c_str(), ov.sdk);
        id = derive_identity(ov);
    } else if (!gen_identity(id, identity_error)) {
        fprintf(stderr, "! cannot generate identity: %s\n", identity_error.c_str());
        return 1;
    }

    if (!old.empty()) {
        Identity old_identity;
        std::string old_error;
        if (load_identity_file(IDENTITY_FILE, old_identity, old_error))
            sbxid::preserve_operational_flags(old_identity.kv, id.kv);
    }
    merge_carrier(id);
    if (!validate_identity(id, identity_error)) {
        fprintf(stderr, "! generated identity rejected: %s\n", identity_error.c_str());
        return 1;
    }
    if (!old.empty() && !atomic_write(IDENTITY_BAK, old)) {
        fprintf(stderr, "! failed to preserve existing identity backup\n");
        return 1;
    }
    if (!atomic_write(IDENTITY_FILE, id.serialize())) {
        fprintf(stderr, "! failed to write identity.prop\n");
        return 1;
    }

    generate_mount_files(id);

    printf("OK - fresh persona stored locally\n");
    printf("  MODEL       : %s\n", id.kv["MODEL"].c_str());
    printf("  DEVICE      : %s\n", id.kv["DEVICE"].c_str());
    printf("  RELEASE     : %s (SDK %s)\n",
           id.kv["RELEASE"].c_str(), id.kv["SDK_INT"].c_str());
    printf("  FINGERPRINT : %s\n", id.kv["FINGERPRINT"].c_str());
    printf("  SERIAL      : %s\n", id.kv["SERIAL"].c_str());
    printf("  ENTROPY ID  : %s (profile metadata; not an SSAID API result)\n",
           id.kv["ANDROID_ID"].c_str());
    printf("  LOCAL GAID  : %s (desired storage value; service API remains genuine)\n",
           id.kv["GOOGLE_AID"].c_str());
    printf("  SEC PATCH   : %s\n", id.kv["SECURITY_PATCH"].c_str());
    printf("  HOST        : %s\n", id.kv["HOST"].c_str());
    printf("  RADIO       : %s\n", id.kv["RADIO"].c_str());

    printf("  Restart target apps manually; no app data was cleared.\n");
    return 0;
}

static int cmd_import(const char* path) {
    if (!ensure_root()) return 1;
    if (!path || !*path) {
        fprintf(stderr, "Usage: sandboxid import <identity-file>\n");
        return 2;
    }

    Identity candidate;
    std::string error;
    if (!load_identity_file(path, candidate, error)) {
        fprintf(stderr, "! imported identity rejected: %s\n", error.c_str());
        return 1;
    }

    const std::string old = read_file(IDENTITY_FILE);
    if (!old.empty()) {
        Identity current;
        std::string current_error;
        if (load_identity_file(IDENTITY_FILE, current, current_error))
            sbxid::preserve_operational_flags(current.kv, candidate.kv);
    }
    merge_carrier(candidate);
    if (!validate_identity(candidate, error)) {
        fprintf(stderr, "! imported identity rejected: %s\n", error.c_str());
        return 1;
    }
    if (!old.empty() && !atomic_write(IDENTITY_BAK, old)) {
        fprintf(stderr, "! failed to preserve existing identity backup\n");
        return 1;
    }
    if (!atomic_write(IDENTITY_FILE, candidate.serialize())) {
        fprintf(stderr, "! failed to import identity.prop\n");
        return 1;
    }
    generate_mount_files(candidate);
    printf("OK: imported module-local identity; restart target apps manually\n");
    return 0;
}

static int cmd_status() {
    std::string d = read_file(IDENTITY_FILE);
    if (d.empty()) {
        printf("no identity yet - run `sandboxid freshen`\n");
        return 0;
    }
    fputs(d.c_str(), stdout);
    return 0;
}

static bool load_current_identity(Identity& id);

static bool operational_flag(const char* key) {
    return key && sbxid::operational_flag_key(key);
}

static int cmd_set_flag(const char* key, const char* value) {
    if (!ensure_root()) return 1;
    if (!key || !value || !operational_flag(key) ||
        (strcmp(value, "0") && strcmp(value, "1"))) {
        fprintf(stderr, "Usage: sandboxid set-flag <SBX_* flag> <0|1>\n");
        return 2;
    }
    Identity id;
    if (!load_current_identity(id)) return 1;
    id.kv[key] = value;
    std::string error;
    if (!validate_identity(id, error)) {
        fprintf(stderr, "! updated identity rejected: %s\n", error.c_str());
        return 1;
    }
    if (!atomic_write(IDENTITY_FILE, id.serialize())) {
        fprintf(stderr, "! failed to write identity.prop\n");
        return 1;
    }
    printf("OK: %s=%s (restart target app to apply)\n", key, value);
    return 0;
}

static bool load_current_identity(Identity& id) {
    std::string error;
    if (load_identity_file(IDENTITY_FILE, id, error)) return true;
    fprintf(stderr, "! identity rejected: %s\n", error.c_str());
    return false;
}

static int cmd_retired_device_wide(const char* command) {
    if (!ensure_root()) return 1;
    fprintf(stderr,
            "! %s retired: SandboxID no longer mutates device-wide properties "
            "or framework Settings. Use seed/freshen and restart target apps.\n",
            command ? command : "command");
    return 2;
}

static int cmd_seed() {
    if (!ensure_root()) return 1;
    Identity id;
    std::string error;
    std::string existing = read_file(IDENTITY_FILE);
    if (!existing.empty()) {
        bool needs_migration = false;
        if (!load_identity_file(IDENTITY_FILE, id, error, &needs_migration)) {
            fprintf(stderr, "! seed: existing identity rejected: %s\n", error.c_str());
            return 1;
        }
        if (needs_migration) {
            if (!atomic_write(IDENTITY_FILE, id.serialize())) {
                fprintf(stderr, "! seed: failed to migrate legacy identity\n");
                return 1;
            }
            DBG("seed: migrated legacy identity to presentation-only format");
        }
        DBG("seed: reusing existing identity (%zu keys)", id.kv.size());
    } else {
        DBG("seed: no identity yet, generating fresh");
        if (!gen_identity(id, error)) {
            fprintf(stderr, "! seed: cannot generate identity: %s\n", error.c_str());
            return 1;
        }
        merge_carrier(id);
        if (!validate_identity(id, error)) {
            fprintf(stderr, "! seed: generated identity rejected: %s\n", error.c_str());
            return 1;
        }
        if (!atomic_write(IDENTITY_FILE, id.serialize())) {
            fprintf(stderr, "! seed: failed to write %s\n", IDENTITY_FILE);
            return 1;
        }
    }
    generate_mount_files(id);
    printf("OK: seed complete (mount overlay ready at %s)\n", MOUNTDIR);
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

static int cmd_rollback() {
    if (!ensure_root()) return 1;
    Identity rid;
    std::string error;
    if (!load_identity_file(IDENTITY_BAK, rid, error)) {
        fprintf(stderr, "! backup rejected: %s\n", error.c_str());
        return 1;
    }
    Identity current;
    std::string current_error;
    if (load_identity_file(IDENTITY_FILE, current, current_error))
        sbxid::preserve_operational_flags(current.kv, rid.kv);
    if (!validate_identity(rid, error)) {
        fprintf(stderr, "! restored identity rejected: %s\n", error.c_str());
        return 1;
    }
    if (!atomic_write(IDENTITY_FILE, rid.serialize())) {
        fprintf(stderr, "! failed to restore identity backup\n");
        return 1;
    }
    generate_mount_files(rid);
    printf("OK: rolled back locally; restart target apps manually\n");
    return 0;
}

static int cmd_applog_ids(const char* pkg) {
    if (!pkg || !*pkg) {
        fprintf(stderr, "applog-ids: butuh nama package\n");
        return 2;
    }
    Identity id;
    std::string error;
    if (!load_identity_file(IDENTITY_FILE, id, error)) {
        fprintf(stderr, "! identity rejected: %s\n", error.c_str());
        return 1;
    }
    auto g = [&](const char* k) -> std::string {
        auto it = id.kv.find(k);
        return it != id.kv.end() ? it->second : std::string();
    };
    uint64_t epoch_ms = strtoull(g("APPLOG_EPOCH").c_str(), nullptr, 10);
    if (epoch_ms == 0) epoch_ms = 1700000000000ULL;
    uint64_t seed = sbxnr::fnv1a(g("FINGERPRINT") + "|" + g("SERIAL") + "|" +
                                 g("ANDROID_ID") + "|" + std::string(pkg));
    sbxnr::ApplogIds ids = sbxnr::make_applog_ids(seed, epoch_ms);
    printf("PKG=%s\n",        pkg);
    printf("EPOCH=%llu\n",    (unsigned long long)epoch_ms);
    printf("DID=%s\n",        ids.did.c_str());
    printf("IID=%s\n",        ids.iid.c_str());
    printf("SSID=%s\n",       ids.ssid.c_str());
    printf("OPENUDID=%s\n",   ids.openudid.c_str());
    printf("CLIENTUDID=%s\n", ids.clientudid.c_str());
    printf("CDID=%s\n",       ids.cdid.c_str());
    return 0;
}

static void usage(const char* p) {
    fprintf(stderr,
        "SandboxID — Android device identifier privacy research module\n\n"
        "Usage: %s <command>\n\n"
        "  freshen      Generate and store a module-local persona\n"
        "  import <file> Validate and atomically import a module-local persona\n"
        "  status       Print current identity.prop\n"
        "  set-flag <key> <0|1>\n"
        "               Set one operational SBX_* flag atomically\n"
        "  rollback     Restore previous module-local identity from backup\n"
        "  lock         Prevent freshen (safety)\n"
        "  unlock       Re-enable freshen\n"
        "  apply-props  Retired compatibility command (never mutates)\n"
        "  apply-boot   Retired compatibility command (never mutates)\n"
        "  seed         Validate/generate identity + target mount files\n"
        "  targets      List current target packages from target.txt\n"
        "  applog-ids <pkg>\n"
        "               Print the AppLog IDs the L9 hook serves for <pkg>\n",
        p);
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const char* c = argv[1];
    if (!strcmp(c, "freshen"))    return cmd_freshen();
    if (!strcmp(c, "import"))     return cmd_import(argc > 2 ? argv[2] : nullptr);
    if (!strcmp(c, "status"))     return cmd_status();
    if (!strcmp(c, "set-flag"))   return cmd_set_flag(argc > 2 ? argv[2] : nullptr,
                                                        argc > 3 ? argv[3] : nullptr);
    if (!strcmp(c, "rollback"))   return cmd_rollback();
    if (!strcmp(c, "lock"))       return cmd_lock();
    if (!strcmp(c, "unlock"))     return cmd_unlock();
    if (!strcmp(c, "apply-props")) return cmd_retired_device_wide(c);
    if (!strcmp(c, "apply-boot")) return cmd_retired_device_wide(c);
    if (!strcmp(c, "seed"))       return cmd_seed();
    if (!strcmp(c, "targets"))    return cmd_targets();
    if (!strcmp(c, "applog-ids")) return cmd_applog_ids(argc > 2 ? argv[2] : nullptr);
    usage(argv[0]);
    return 1;
}
