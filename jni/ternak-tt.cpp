// ============================================================
// Ternak TT v1.0.1 - Standalone CLI (no daemon required)
//
// Semua logic freshen jalan di sini. Dipanggil user via `su`,
// jadi udah root. Gak butuh UDS/daemon lagi.
// ============================================================
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
#include "pool_tt.hpp"

static const char* MODDIR         = "/data/adb/modules/ternak_tt";
static const char* IDENTITY_FILE  = "/data/adb/modules/ternak_tt/identity.prop";
static const char* IDENTITY_BAK   = "/data/adb/modules/ternak_tt/identity.prop.bak";
static const char* MODE_FILE      = "/data/adb/modules/ternak_tt/identity.mode";
static const char* RESETPROP      = "/data/adb/modules/ternak_tt/bin/resetprop-rs";
static const char* MOUNTDIR       = "/data/adb/modules/ternak_tt/mount";  // v1.0.3

static const char* TT_PACKAGES[] = {
    "com.zhiliaoapp.musically",
    "com.ss.android.ugc.trill",
    "com.zhiliaoapp.musically.go",
};

// ---- Helpers ----
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

// ---- Identity ----
struct Identity {
    std::map<std::string, std::string> kv;
    std::string serialize() const {
        static const std::vector<std::string> order = {
            "BRAND","MANUFACTURER","MODEL","DEVICE","PRODUCT",
            "BOARD","HARDWARE","FINGERPRINT","ID","DISPLAY","DESCRIPTION",
            "BOOTLOADER","HOST","USER","TYPE","TAGS",
            "INCREMENTAL","RELEASE","SDK_INT","SECURITY_PATCH",
            "SERIAL","RADIO","ANDROID_ID","GOOGLE_AID",
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
    id.kv["BOOTLOADER"]      = "unknown";
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

    std::time_t now = std::time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    char date[16];
    strftime(date, sizeof(date), "%y%m%d", &lt);
    char rad[128];
    snprintf(rad, sizeof(rad), "g5300q-%s-%s-B-%s", date, date, p.incremental);
    id.kv["RADIO"] = rad;

    id.kv["SERIAL"]     = random_hex(8, true);   // 16 hex upper
    id.kv["ANDROID_ID"] = random_hex(8, false);  // 16 hex lower
    id.kv["GOOGLE_AID"] = uuid_v4();
    return id;
}

// v1.0.8: variant tag + debug stderr traces (compiled out in release)
#ifdef TT_DEBUG
#define TT_VARIANT_TAG "debug"
#define DBG(fmt, ...) fprintf(stderr, "[D] " fmt "\n", ##__VA_ARGS__)
#else
#define TT_VARIANT_TAG "release"
#define DBG(...) ((void)0)
#endif

// ---- Apply native + wipe ----
// v1.0.2: cover full Build.* mapping + partitioned props (Android 12+)
// biar app yang bypass Java Build.* via JNI __system_property_get langsung
// tetap ke-spoof, dan gak ada mismatch antara Java view vs native view.
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
    const std::string RADIO        = get("RADIO");
    const std::string TAGS         = get("TAGS");
    const std::string TYPE         = get("TYPE");
    const std::string USER_        = get("USER");
    const std::string HOST         = get("HOST");

    std::vector<Rp> rp = {
        // Serial
        {"ro.serialno",                        SERIAL},
        {"ro.boot.serialno",                   SERIAL},

        // Fingerprint (main + partition aliases)
        {"ro.build.fingerprint",               FP},
        {"ro.bootimage.build.fingerprint",     FP},
        {"ro.system.build.fingerprint",        FP},
        {"ro.vendor.build.fingerprint",        FP},
        {"ro.odm.build.fingerprint",           FP},
        {"ro.product.build.fingerprint",       FP},
        {"ro.system_ext.build.fingerprint",    FP},

        // Model / Brand / Manufacturer / Device / Product / Board
        // (top-level + all partition aliases Android 12+)
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

        // Build.ID / DISPLAY / DESCRIPTION / TAGS / TYPE / USER / HOST
        {"ro.build.id",                        ID_},
        {"ro.build.display.id",                DISPLAY},
        {"ro.build.description",               DESC},
        {"ro.build.tags",                      TAGS},
        {"ro.build.type",                      TYPE},
        {"ro.build.user",                      USER_},
        {"ro.build.host",                      HOST},

        // Version
        {"ro.build.version.release",           RELEASE},
        {"ro.build.version.release_or_codename", RELEASE},
        {"ro.build.version.sdk",               SDK_INT},
        {"ro.system.build.version.sdk",        SDK_INT},
        {"ro.vendor.build.version.sdk",        SDK_INT},
        {"ro.build.version.security_patch",    SECPATCH},
        {"ro.vendor.build.security_patch",     SECPATCH},
        {"ro.build.version.incremental",       INCREMENTAL},

        // Radio / baseband
        {"gsm.version.baseband",               RADIO},
        {"ro.build.expect.baseband",           RADIO},

        // Bootloader (Pixel-style)
        {"ro.bootloader",                      std::string("unknown")},
        {"ro.boot.bootloader",                 std::string("unknown")},
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

    // Settings provider (survives reboot)
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

// ============================================================
// v1.0.3: Generate fake files for Zygisk bind-mount overlay
// - 5 build.prop per partition (system, vendor, odm, product, system_ext)
// - 1 settings_secure.xml with spoofed android_id + advertising_id
// Files are consumed by main.cpp:do_bind_mounts() at TT app launch.
// ============================================================
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

    // Base build.prop (all key=value lines)
    std::string base;
    base += "# Ternak TT synthetic build.prop (v1.0.3)\n";
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
        c += "# Partition alias (" + std::string(p.dir) + ")\n";
        if (!MODEL.empty())        c += pfx + "model="        + MODEL        + "\n";
        if (!BRAND.empty())        c += pfx + "brand="        + BRAND        + "\n";
        if (!MANUFACTURER.empty()) c += pfx + "manufacturer=" + MANUFACTURER + "\n";
        if (!DEVICE.empty())       c += pfx + "device="       + DEVICE       + "\n";
        if (!PRODUCT.empty())      c += pfx + "name="         + PRODUCT      + "\n";
        std::string path = std::string(MOUNTDIR) + "/" + p.dir + "/build.prop";
        atomic_write(path, c);
        ::chmod(path.c_str(), 0644);
    }

    // settings_secure.xml — minimal Android SettingsProvider format
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
    atomic_write(xml_path, xml);
    // Mirror real DAC (0600 system:system). TT can't read even our fake
    // via normal file APIs, but if a privesc exploit lets them, they get
    // spoofed data instead of the real ANDROID_ID.
    ::chmod(xml_path.c_str(), 0600);
    ::chown(xml_path.c_str(), 1000, 1000);  // system:system

    // Restore SELinux labels so bind mount doesn't get denied on target read.
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

    printf("  Mount overlay: 5 build.prop + settings_secure.xml -> %s\n", MOUNTDIR);
}

static void wipe_tt_data() {
    for (const char* pkg : TT_PACKAGES) {
        run_bin("/system/bin/pm", {"pm", "clear", pkg});
        run_bin("/system/bin/am", {"am", "force-stop", pkg});
    }
}

static Identity load_identity() {
    Identity id;
    std::istringstream iss(read_file(IDENTITY_FILE));
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        id.kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return id;
}

// ---- Root check ----
static bool ensure_root() {
    if (geteuid() != 0) {
        fprintf(stderr, "! ternak-tt must run as root. Use: su -c ternak-tt <cmd>\n");
        return false;
    }
    return true;
}

// ---- Command handlers ----
static int cmd_freshen() {
    DBG("cmd_freshen: build=%s", TT_VARIANT_TAG);
    if (!ensure_root()) return 1;

    std::string mode = trim(read_file(MODE_FILE));
    if (mode == "locked") {
        printf("LOCKED: run `ternak-tt unlock` first\n");
        return 1;
    }

    std::string old = read_file(IDENTITY_FILE);
    if (!old.empty()) atomic_write(IDENTITY_BAK, old);

    Identity id = gen_identity();
    if (!atomic_write(IDENTITY_FILE, id.serialize())) {
        fprintf(stderr, "! failed to write identity.prop\n");
        return 1;
    }

    apply_native(id);
    generate_mount_files(id);  // v1.0.3: refresh Zygisk overlay files
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
    printf("  SEC PATCH   : %s\n", id.kv["SECURITY_PATCH"].c_str());
    printf("  Wiped: musically, trill, musically.go\n");
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
    generate_mount_files(id);  // v1.0.3: refresh overlay on boot
    printf("OK: native prop re-applied + mount overlay refreshed\n");
    return 0;
}

// v1.0.14: seed = early-boot bootstrap for post-fs-data.sh.
// Runs BEFORE Android userspace / Zygisk. Only does file writes:
//   * generate identity.prop if missing (reuse existing otherwise)
//   * regenerate mount/*/build.prop tree so companion bind-mount has
//     valid sources on the very first target spawn.
// Deliberately DOES NOT call apply_native() or wipe_tt_data(): resetprop
// and pm/am/settings binaries are unusable this early. apply-boot picks
// those up later from service.sh once boot_completed.
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
    std::string d = read_file(IDENTITY_BAK);
    if (d.empty()) {
        printf("no backup\n");
        return 1;
    }
    atomic_write(IDENTITY_FILE, d);
    Identity rid = load_identity();
    apply_native(rid);
    generate_mount_files(rid);  // v1.0.3: refresh overlay after rollback
    wipe_tt_data();
    printf("OK: rolled back + wiped\n");
    return 0;
}

static void usage(const char* p) {
    fprintf(stderr,
        "Ternak TT v1.0.1 - TikTok Zygisk fresh persona (standalone)\n\n"
        "Usage: %s <command>\n\n"
        "  freshen      Rotate identity + wipe TT app data (main action)\n"
        "  status       Print current identity.prop\n"
        "  rollback     Restore previous identity from backup\n"
        "  lock         Prevent freshen (safety)\n"
        "  unlock       Re-enable freshen\n"
        "  apply-boot   Re-apply native prop (used by service.sh)\n"
        "  seed         Fast bootstrap: identity + mount overlay only\n"
        "               (used by post-fs-data.sh, no native/wipe)\n",
        p);
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const char* c = argv[1];
    if (!strcmp(c, "freshen"))    return cmd_freshen();
    if (!strcmp(c, "status"))     return cmd_status();
    if (!strcmp(c, "rollback"))   return cmd_rollback();
    if (!strcmp(c, "lock"))       return cmd_lock();
    if (!strcmp(c, "unlock"))     return cmd_unlock();
    if (!strcmp(c, "apply-boot")) return cmd_apply_boot();
    if (!strcmp(c, "seed"))       return cmd_seed();  // v1.0.14: early bootstrap
    usage(argv[0]);
    return 1;
}
