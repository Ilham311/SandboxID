// ============================================================================
// Ternak TT CLI — identity rotation, seed, apply-boot
// Fix v1.1: SDK pinning, RADIO format, run_bin exit status, atomic_write partial-io
// ============================================================================
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
#include "tt_config.hpp"
#include <sys/system_properties.h>

static const char* MODDIR         = tt::MODDIR;
static const char* IDENTITY_FILE  = tt::IDENTITY_FILE;
static const char* IDENTITY_BAK   = tt::IDENTITY_BAK;
static const char* MODE_FILE      = tt::MODE_FILE;
static const char* RESETPROP      = tt::RESETPROP;
static const char* MOUNTDIR       = tt::MOUNTDIR;
static const char* TARGET_FILE    = tt::TARGET_FILE;

// ============================================================================
// Utility functions
// ============================================================================

// Load target packages from target.txt; fallback ke built-in defaults.
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

// Generate random hex string dari CSPRNG (std::random_device).
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

// Write file atomically: tmp unik + fsync + rename + fsync direktori induk.
// M2: suffix unik (pid) + O_EXCL mencegah tabrakan / pre-seed symlink antar writer
//     bersamaan; fsync direktori induk membuat entri rename durable terhadap crash.
static bool atomic_write(const std::string& p, const std::string& data) {
    std::string tmp = p + ".tmp." + std::to_string((long)::getpid());
    ::unlink(tmp.c_str());  // buang sisa dari writer sebelumnya dengan pid sama
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
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
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
    return true;
}

// Read file contents; return "" on failure.
static std::string read_file(const std::string& p) {
    std::ifstream f(p);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Trim trailing newline/CR/space and leading whitespace.
static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    size_t st = s.find_first_not_of(" \t");
    if (st != std::string::npos) s = s.substr(st);
    return s;
}

// Run binary synchronously and return exit status (supaya caller tahu gagal).
static int run_bin(const char* path, std::vector<const char*> argv) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        argv.push_back(nullptr);
        execv(path, const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// Seperti run_bin tapi cari di PATH (execvp) — untuk applet 'resetprop' sistem.
static int run_bin_path(const char* file, std::vector<const char*> argv) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        argv.push_back(nullptr);
        execvp(file, const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// ============================================================================
// Identity generation
// ============================================================================
struct Identity {
    std::map<std::string, std::string> kv;
    // Serialize dengan key order yang konsisten untuk readability.
    std::string serialize() const {
        static const std::vector<std::string> order = {
            "BRAND","MANUFACTURER","MODEL","MARKETNAME","DEVICE","PRODUCT",
            "BOARD","HARDWARE","BOARD_PLATFORM","FINGERPRINT","ID","DISPLAY","DESCRIPTION",
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

// Generate UUID v4 dari CSPRNG.
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

// Baca SDK level device dari system properties.
static int device_sdk() {
    char b[PROP_VALUE_MAX] = {0};
    if (__system_property_get("ro.build.version.sdk", b) > 0) return atoi(b);
    return 0;
}

// Generate random build host suffix supaya farm tidak share satu nilai HOST
// (clustering amplifier server-side).
static std::string gen_host_suffix() {
    std::random_device rd;
    std::mt19937 g(rd());
    static const char* prefixes[] = {
        "abfarm", "abfarm-release", "abfarm-server", "build", "build-server",
        "release", "release-server", "farm", "buildfarm",
    };
    constexpr int n_prefixes = sizeof(prefixes) / sizeof(prefixes[0]);
    std::string host = prefixes[g() % n_prefixes];
    // Tambahkan suffix numeric untuk uniqueness per-device
    host += "-";
    host += std::to_string(g() % 900 + 100);  // 3-digit 100-999
    return host;
}

// Generate fresh identity dari pool dengan SDK pinning.
// SDK_INT ter-inline javac → persona SDK HARUS == device SDK supaya konsisten.
static Identity gen_identity() {
    std::random_device rd;
    std::mt19937 g(rd());
    constexpr size_t N = sizeof(TT_POOL) / sizeof(TT_POOL[0]);

    int dev = device_sdk();

    // Prioritas 1: persona SDK persis == device SDK (wajib untuk konsistensi)
    std::vector<size_t> cand;
    for (size_t i = 0; i < N; ++i)
        if (dev > 0 && TT_POOL[i].sdk == dev) cand.push_back(i);

    if (cand.empty()) {
        // Prioritas 2: fallback ke SDK lebih rendah (TERIMA RISIKO inkonsistensi SDK_INT)
        fprintf(stderr, "! tidak ada persona SDK %d persis — fallback SDK lebih rendah, "
                "TERIMA RISIKO inkonsistensi SDK_INT (javac inline)\n", dev);
        for (size_t i = 0; i < N; ++i)
            if (dev > 0 && TT_POOL[i].sdk <= dev) cand.push_back(i);
    }

    size_t idx;
    if (!cand.empty()) {
        idx = cand[g() % cand.size()];
    } else {
        // Prioritas 3: device SDK di bawah semua persona (sangat jarang, upgrade path)
        idx = 0;
        for (size_t i = 1; i < N; ++i) if (TT_POOL[i].sdk < TT_POOL[idx].sdk) idx = i;
        fprintf(stderr, "! device SDK %d below all personas; using SDK %d (upgrade, risky)\n",
                dev, TT_POOL[idx].sdk);
    }
    const PixelEntry& p = TT_POOL[idx];

    Identity id;
    id.kv["BRAND"]           = "google";
    id.kv["MANUFACTURER"]    = "Google";
    id.kv["MODEL"]           = p.model;
    id.kv["MARKETNAME"]      = p.model;
    id.kv["DEVICE"]          = p.device;
    id.kv["PRODUCT"]         = p.product;
    id.kv["BOARD"]           = p.board;
    id.kv["HARDWARE"]        = p.board;
    id.kv["BOARD_PLATFORM"]  = p.platform;
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

    // FINGERPRINT: google/<product>/<device>:<release>/<id>/<incremental>:user/release-keys
    char fp[512];
    snprintf(fp, sizeof(fp), "google/%s/%s:%s/%s/%s:user/release-keys",
             p.product, p.device, p.release, p.id, p.incremental);
    id.kv["FINGERPRINT"] = fp;
    id.kv["DISPLAY"]     = p.id;

    char desc[512];
    snprintf(desc, sizeof(desc), "%s-user %s %s %s release-keys",
             p.product, p.release, p.id, p.incremental);
    id.kv["DESCRIPTION"] = desc;

    // RADIO: format baseband Pixel nyata <modem>-<YYMMDD>-B-<incremental>
    // (tanpa segmen incr6 ekstra yang adalah anomali bagi pembanding)
    auto modem_prefix = [](const char* plat) -> const char* {
        if (!plat) return "g5123b";
        if (!strcmp(plat, "gs101"))   return "g5123b";
        if (!strcmp(plat, "gs201"))   return "g5300b";
        if (!strcmp(plat, "zuma"))    return "g5300q";
        if (!strcmp(plat, "zumapro")) return "g5400";
        if (!strcmp(plat, "laguna"))  return "g5500";
        return "g5123b";
    };
    char pdate[8] = "000000";
    if (std::strlen(p.security_patch) >= 10) {
        pdate[0] = p.security_patch[2]; pdate[1] = p.security_patch[3];
        pdate[2] = p.security_patch[5]; pdate[3] = p.security_patch[6];
        pdate[4] = p.security_patch[8]; pdate[5] = p.security_patch[9];
    }
    char rad[128];
    snprintf(rad, sizeof(rad), "%s-%s-B-%s",
             modem_prefix(p.platform), pdate, p.incremental);
    id.kv["RADIO"] = rad;

    // Serial: 8 hex uppercase (format Pixel)
    id.kv["SERIAL"]     = random_hex(8, true);
    id.kv["ANDROID_ID"] = random_hex(8, false);
    id.kv["GOOGLE_AID"] = uuid_v4();
    return id;
}

#ifdef TT_DEBUG
#define TT_VARIANT_TAG "debug"
#define DBG(fmt, ...) fprintf(stderr, "[D] " fmt "\n", ##__VA_ARGS__)
#else
#define TT_VARIANT_TAG "release"
#define DBG(...) ((void)0)
#endif

// ============================================================================
// L0 — Native prop application via resetprop-rs (global, boot persistent)
// ============================================================================
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
    const std::string SECPATCH     = get("SECURITY_PATCH");
    const std::string INCREMENTAL  = get("INCREMENTAL");
    const std::string RADIO        = get("RADIO");
    const std::string TAGS         = get("TAGS");
    const std::string TYPE         = get("TYPE");
    const std::string USER_        = get("USER");
    const std::string HOST         = get("HOST");
    const std::string HARDWARE     = get("HARDWARE");
    const std::string PLATFORM     = get("BOARD_PLATFORM");
    const std::string MARKETNAME   = get("MARKETNAME");

    // NOTE: ro.build.version.sdk TIDAK di-resetprop global.
    // Build.VERSION.SDK_INT ter-inline javac; meresetprop nilai global-nya membuat
    // seluruh sistem melihat SDK yang salah (GMS, update engine, kompat layanan).
    // Per-app spoof SDK_INT sudah ditangani oleh Build field patch di main.cpp.
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

        {"ro.hardware",                        HARDWARE},
        {"ro.board.platform",                  PLATFORM},
        {"ro.product.marketname",              MARKETNAME},

        {"ro.build.id",                        ID_},
        {"ro.build.display.id",                DISPLAY},
        {"ro.build.description",               DESC},
        {"ro.build.tags",                      TAGS},
        {"ro.build.type",                      TYPE},
        {"ro.build.user",                      USER_},
        {"ro.build.host",                      HOST},

        {"ro.build.version.release",           RELEASE},
        {"ro.build.version.release_or_codename", RELEASE},
        {"ro.build.version.security_patch",    SECPATCH},
        {"ro.vendor.build.security_patch",     SECPATCH},
        {"ro.build.version.incremental",       INCREMENTAL},

        {"gsm.version.baseband",               RADIO},
        {"ro.build.expect.baseband",           RADIO},

        {"ro.bootloader",                      std::string("unknown")},
        {"ro.boot.bootloader",                 std::string("unknown")},
    };

    // M6: resetprop-rs bundled = arm64-only. Bila absen (ABI lain / dibuang saat
    // install), fallback ke applet 'resetprop' (Magisk) lalu 'resetprop-rs' di PATH
    // supaya prop native tetap ter-apply. Jalur arm64 (bundled ada) tak berubah.
    bool have_bundled = (::access(RESETPROP, X_OK) == 0);
    {
        int applied = 0, failed = 0;
        for (const auto& r : rp) {
            if (r.val.empty()) continue;
            int rc;
            if (have_bundled) {
                rc = run_bin(RESETPROP, {"resetprop-rs", "-n", r.key, r.val.c_str()});
            } else {
                rc = run_bin_path("resetprop", {"resetprop", "-n", r.key, r.val.c_str()});
                if (rc != 0)
                    rc = run_bin_path("resetprop-rs", {"resetprop-rs", "-n", r.key, r.val.c_str()});
            }
            if (rc == 0) {
                applied++;
            } else {
                failed++;
                fprintf(stderr, "! resetprop gagal (exit!=0): %s\n", r.key);
            }
        }
        printf("  Native prop: %d ok, %d gagal%s\n", applied, failed,
               have_bundled ? "" : " [fallback PATH]");
        if (applied == 0 && failed > 0)
            fprintf(stderr, "! SEMUA resetprop gagal%s — cek ketersediaan resetprop / resetprop-rs\n",
                    have_bundled ? "" : " (bundled absent + PATH fallback gagal)");
    }

    // Settings: ANDROID_ID hanya untuk Settings.Global, bukan SSAID per-app
    // (yang tidak bisa di-set via settings put — lihat wipe_ssaid di rotate_ids.sh)
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

// ============================================================================
// L4 — Mount overlay file generation (bind-mount sources)
// ============================================================================
static void generate_mount_files(const Identity& id) {
    DBG("generate_mount_files: MOUNTDIR=%s", MOUNTDIR);
    auto g = [&](const char* k) -> std::string {
        auto it = id.kv.find(k);
        return it != id.kv.end() ? it->second : std::string();
    };

    ::mkdir(MOUNTDIR, 0755);
    for (size_t i = 0; i < tt::MOUNT_PARTS_N; ++i) {
        std::string d = std::string(MOUNTDIR) + "/" + tt::MOUNT_PARTS[i];
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
    const std::string HARDWARE     = g("HARDWARE");
    const std::string PLATFORM     = g("BOARD_PLATFORM");
    const std::string MARKETNAME   = g("MARKETNAME");

    // build.prop base — semua prop identitas untuk file-reader
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
    add("ro.product.model",                   MODEL);
    add("ro.product.brand",                   BRAND);
    add("ro.product.manufacturer",            MANUFACTURER);
    add("ro.product.device",                  DEVICE);
    add("ro.product.name",                    PRODUCT);
    add("ro.product.board",                   BOARD);
    add("ro.hardware",                        HARDWARE);
    add("ro.board.platform",                  PLATFORM);
    add("ro.product.marketname",              MARKETNAME);
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

    // Per-partition build.prop (system, vendor, odm, product, system_ext)
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
        std::string path = std::string(MOUNTDIR) + "/" + p.dir + "/build.prop";
        atomic_write(path, c);
        ::chmod(path.c_str(), 0644);
    }

    // settings_secure.xml untuk bind-mount ke /data/system/users/0/
    // (dibaca oleh SettingsProvider saat system_server belum warm —
    //  untuk SSAID per-app ini tidak efektif, tapi berguna untuk
    //  file-reader yang membaca settings_secure.xml langsung)
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

    ::chmod(xml_path.c_str(), 0600);
    ::chown(xml_path.c_str(), 1000, 1000);

    // SELinux context labels supaya file overlay bisa dibaca oleh target process
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

// ============================================================================
// App data wipe
// ============================================================================
// Clear app data untuk semua target packages (fresh device identity).
static void wipe_tt_data() {
    auto pkgs = load_targets();
    for (const auto& pkg : pkgs) {
        run_bin("/system/bin/pm", {"pm", "clear", pkg.c_str()});
        run_bin("/system/bin/am", {"am", "force-stop", pkg.c_str()});
    }
}

// ============================================================================
// CLI Commands
// ============================================================================
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

// Load identity.prop dan trim whitespace/CRLF per value.
static Identity load_identity() {
    Identity id;
    std::istringstream iss(read_file(IDENTITY_FILE));
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = line.substr(eq + 1);
        while (!v.empty() && (v.back()=='\r' || v.back()=='\n' || v.back()==' '))
            v.pop_back();
        if (!k.empty()) id.kv[k] = v;
    }
    return id;
}

// Pastikan berjalan sebagai root.
static bool ensure_root() {
    if (geteuid() != 0) {
        fprintf(stderr, "! ternak-tt must run as root. Use: su -c ternak-tt <cmd>\n");
        return false;
    }
    return true;
}

// Rotate identity: generate fresh persona + apply native + mount overlay + wipe.
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
    printf("  SEC PATCH   : %s\n", id.kv["SECURITY_PATCH"].c_str());
    printf("  HOST        : %s\n", id.kv["HOST"].c_str());
    printf("  RADIO       : %s\n", id.kv["RADIO"].c_str());

    auto pkgs = load_targets();
    printf("  Wiped: %zu pkg(s) from target.txt\n", pkgs.size());
    for (const auto& p : pkgs) printf("    - %s\n", p.c_str());
    return 0;
}

// Print current identity.prop.
static int cmd_status() {
    std::string d = read_file(IDENTITY_FILE);
    if (d.empty()) {
        printf("no identity yet - run `ternak-tt freshen`\n");
        return 0;
    }
    fputs(d.c_str(), stdout);
    return 0;
}

// Re-apply native props + regenerate mount overlay (used by service.sh).
static int cmd_apply_boot() {
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

// Fast bootstrap: identity + mount overlay only (used by post-fs-data.sh).
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

// Lock freshen (safety: prevent accidental rotation).
static int cmd_lock() {
    if (!ensure_root()) return 1;
    atomic_write(MODE_FILE, "locked\n");
    printf("OK: locked\n");
    return 0;
}

// Unlock freshen.
static int cmd_unlock() {
    if (!ensure_root()) return 1;
    atomic_write(MODE_FILE, "fresh\n");
    printf("OK: unlocked\n");
    return 0;
}

// Restore previous identity from backup + re-apply + wipe.
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
    generate_mount_files(rid);
    wipe_tt_data();
    printf("OK: rolled back + wiped\n");
    return 0;
}

// Print usage help.
static void usage(const char* p) {
    fprintf(stderr,
        "Ternak TT v1.1.0 - TikTok Zygisk fresh persona (standalone)\n\n"
        "Usage: %s <command>\n\n"
        "  freshen      Rotate identity + wipe TT app data (main action)\n"
        "  status       Print current identity.prop\n"
        "  rollback     Restore previous identity from backup\n"
        "  lock         Prevent freshen (safety)\n"
        "  unlock       Re-enable freshen\n"
        "  apply-boot   Re-apply native prop (used by service.sh)\n"
        "  seed         Fast bootstrap: identity + mount overlay only\n"
        "               (used by post-fs-data.sh, no native/wipe)\n"
        "  targets      List current target packages from target.txt\n",
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
    if (!strcmp(c, "seed"))       return cmd_seed();
    if (!strcmp(c, "targets"))    return cmd_targets();
    usage(argv[0]);
    return 1;
}