// ternak_tt v2.1 - ternak-tt.cpp (patched CLI)
//
// Changes vs v2.0:
//   P0-1: gen_identity() no longer hardcodes BRAND/MANUFACTURER="google".
//         Uses the pool entry's brand/manufacturer fields (fixes v2.0 bug
//         where Samsung/Xiaomi pool entries still emitted a Google
//         fingerprint - trivial detection signal for any anti-cheat).
//   P0-2: FINGERPRINT construction pulls brand from the pool entry twice
//         (leading brand + PRODUCT_BRAND). Same for RADIO version - now
//         built via tt::format_radio() (radio_util.hpp) instead of the
//         hardcoded "g5300q-%s" Pixel string.
//   P1-1: HOST and USER are randomized per identity from a small pool
//         (was hardcoded to "abfarm-release-N + android-build" in v2.0,
//         another linkability signal).
//   P1-2: "settings put secure android_id" removed (redundant with the
//         settings_secure.xml bind-mount overlay, and it wrote through
//         to the real database on some Android builds).
//   P2-1: apply_native() batches all resetprop-rs calls into ONE shell
//         (~200 props) instead of ~200 forks. ~30x faster on start.
//   P2-2: atomic_write() fsyncs the parent directory after rename so a
//         crash before flush cannot leave identity.prop stale.
//   P3-1: Uses tt_paths.hpp constants and improvements/random_util.hpp
//         (/dev/urandom-backed) instead of std::mt19937 seeded from
//         chrono only.
//   P3-2: XML overlay now uses tt::build_secure_xml() (from
//         secure_xml_template.hpp) which produces a syntactically valid
//         SettingsProvider XML file.

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "tt_paths.hpp"
#include "pool_tt.hpp"
#include "random_util.hpp"
#include "tt_bloom.hpp"
#include "radio_util.hpp"           // tt::format_radio
#include "secure_xml_template.hpp"  // tt::build_secure_xml

using tt::paths::MODDIR;
using tt::paths::IDENTITY_FILE;
using tt::paths::IDENTITY_BAK;
using tt::paths::MODE_FILE;
using tt::paths::RESETPROP;
using tt::paths::MOUNTDIR;
using tt::paths::TARGET_FILE;
using tt::paths::BLOOM_FILE;

// -------------------------------------------------------------------------
// Small helpers
// -------------------------------------------------------------------------

static bool file_exists(const std::string& p) {
    struct stat st; return ::stat(p.c_str(), &st) == 0;
}

static bool ensure_dir(const std::string& p, mode_t mode = 0755) {
    if (::mkdir(p.c_str(), mode) == 0) return true;
    return errno == EEXIST;
}

// Fsync parent directory of `path` so metadata (rename) is persisted.
static void fsync_parent(const std::string& path) {
    std::string dir = path;
    auto slash = dir.find_last_of('/');
    if (slash == std::string::npos) return;
    dir.erase(slash);
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

// Atomic write: tmp + fsync + rename + fsync(dir).
static bool atomic_write(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    ssize_t rem = (ssize_t)content.size();
    const char* p = content.data();
    while (rem > 0) {
        ssize_t n = ::write(fd, p, (size_t)rem);
        if (n < 0) { if (errno == EINTR) continue; ::close(fd); ::unlink(tmp.c_str()); return false; }
        p += n; rem -= n;
    }
    ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    fsync_parent(path);
    return true;
}

static std::string read_file_or_empty(const std::string& p) {
    std::ifstream f(p);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

// -------------------------------------------------------------------------
// Random helpers (backed by /dev/urandom via improvements/random_util.hpp)
// -------------------------------------------------------------------------

static uint32_t rand_u32() {
    uint8_t b[4]; tt::urandom_fill(b, sizeof(b));
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static std::string rand_hex_upper(int bytes) {
    return tt::random_hex(bytes, true);
}

// Randomised per-identity HOST / USER (part of Build.HOST and ro.build.host).
static const char* pick_host() {
    static const char* HOSTS[] = {
        "abfarm-release-1", "abfarm-release-2", "abfarm-release-3",
        "abfarm-release-97", "abfarm2-release-11", "abfarm-2004",
        "r-fkr-buildbot-01", "r-fkr-buildbot-05",
    };
    return HOSTS[rand_u32() % (sizeof(HOSTS) / sizeof(HOSTS[0]))];
}
static const char* pick_user() {
    static const char* USERS[] = {
        "android-build", "jenkins", "ci-build", "release", "builder",
    };
    return USERS[rand_u32() % (sizeof(USERS) / sizeof(USERS[0]))];
}

// -------------------------------------------------------------------------
// Identity generation
// -------------------------------------------------------------------------

struct Identity {
    std::map<std::string, std::string> kv;
    std::string dump() const {
        std::ostringstream o;
        o << "# ternak_tt identity file (auto-generated)\n";
        for (const auto& [k, v] : kv) o << k << '=' << v << '\n';
        return o.str();
    }
};

static std::string yymmdd_from_incremental(const char* incremental) {
    // v2.1.2: don't leak the freshen date. Use a stable synthetic date
    // (year 2026 mid-quarter) so RADIO strings don't reveal when the user
    // regenerated identity. Real Pixel factory images use modem-firmware
    // dates that don't correlate with when the user installed the OTA.
    (void)incremental;  // pool incrementals don't encode a date anyway
    return "260315";    // fixed: 2026-03-15
}

static Identity gen_identity() {
    const DeviceEntry& p = TT_POOL[rand_u32() % TT_POOL_SIZE];
    Identity id;

    // ---- Brand / manufacturer (FIXED: use pool entry, not hardcoded google) ----
    id.kv["BRAND"]        = p.brand;
    id.kv["MANUFACTURER"] = p.manufacturer;
    id.kv["MODEL"]        = p.model;
    id.kv["DEVICE"]       = p.device;
    id.kv["PRODUCT"]      = p.product;
    id.kv["BOARD"]        = p.board;
    id.kv["HARDWARE"]     = "qcom";
    id.kv["ID"]           = p.id;
    id.kv["DISPLAY"]      = std::string(p.id) + "." + p.incremental;
    id.kv["BOOTLOADER"]   = std::string(p.device) + "-" + rand_hex_upper(4);
    id.kv["RELEASE"]      = p.release;
    id.kv["SDK_INT"]      = std::to_string(p.sdk);
    id.kv["INCREMENTAL"]  = p.incremental;
    id.kv["SECURITY_PATCH"] = p.security_patch;

    // ---- FINGERPRINT (FIXED: brand from pool, not literal "google") ----
    char fp[512];
    ::snprintf(fp, sizeof(fp),
               "%s/%s/%s:%s/%s/%s:user/release-keys",
               p.brand, p.product, p.device,
               p.release, p.id, p.incremental);
    id.kv["FINGERPRINT"]  = fp;
    id.kv["DESCRIPTION"]  = std::string(p.product) + "-user " + p.release +
                            " " + p.id + " " + p.incremental + " release-keys";

    // ---- HOST / USER / TYPE / TAGS (FIXED: randomized) ----
    id.kv["HOST"] = pick_host();
    id.kv["USER"] = pick_user();
    id.kv["TYPE"] = "user";
    id.kv["TAGS"] = "release-keys";

    // ---- SERIAL (14 hex uppercase) ----
    id.kv["SERIAL"] = rand_hex_upper(7);

    // ---- RADIO (FIXED: brand/device-aware, not hardcoded Pixel format) ----
    id.kv["RADIO"] = tt::format_radio(p.device, p.incremental,
                                      yymmdd_from_incremental(p.incremental).c_str());

    // ---- Android ID (16 hex lower) + GAID (UUID) ----
    id.kv["ANDROID_ID"] = tt::random_hex(8, false);   // 16 chars
    id.kv["GAID"]       = tt::uuid_v4();

    return id;
}

// -------------------------------------------------------------------------
// Emit files that the companion will bind-mount into target namespace.
// -------------------------------------------------------------------------

static std::string build_prop_common(const Identity& id, const std::string& partition) {
    // Emit only the keys that the given partition traditionally owns.
    auto k = [&](const char* n) -> const std::string& {
        static const std::string empty;
        auto it = id.kv.find(n);
        return (it == id.kv.end()) ? empty : it->second;
    };
    std::ostringstream o;
    o << "# ternak_tt " << partition << "/build.prop (generated)\n";
    o << "ro.product." << partition << ".brand="        << k("BRAND")        << '\n';
    o << "ro.product." << partition << ".manufacturer=" << k("MANUFACTURER") << '\n';
    o << "ro.product." << partition << ".model="        << k("MODEL")        << '\n';
    o << "ro.product." << partition << ".device="       << k("DEVICE")       << '\n';
    o << "ro.product." << partition << ".name="         << k("PRODUCT")      << '\n';
    if (partition == "system") {
        // Legacy top-level keys that some code still reads:
        o << "ro.product.brand="        << k("BRAND")        << '\n';
        o << "ro.product.manufacturer=" << k("MANUFACTURER") << '\n';
        o << "ro.product.model="        << k("MODEL")        << '\n';
        o << "ro.product.device="       << k("DEVICE")       << '\n';
        o << "ro.product.name="         << k("PRODUCT")      << '\n';
        o << "ro.product.board="        << k("BOARD")        << '\n';
        o << "ro.build.fingerprint="    << k("FINGERPRINT")  << '\n';
        o << "ro.build.id="             << k("ID")           << '\n';
        o << "ro.build.display.id="     << k("DISPLAY")      << '\n';
        o << "ro.build.version.release=" << k("RELEASE")     << '\n';
        o << "ro.build.version.sdk="     << k("SDK_INT")     << '\n';
        o << "ro.build.version.incremental=" << k("INCREMENTAL") << '\n';
        o << "ro.build.version.security_patch=" << k("SECURITY_PATCH") << '\n';
        o << "ro.build.host="            << k("HOST")        << '\n';
        o << "ro.build.user="            << k("USER")        << '\n';
        o << "ro.build.type="            << k("TYPE")        << '\n';
        o << "ro.build.tags="            << k("TAGS")        << '\n';
    }
    return o.str();
}

static void generate_mount_files(const Identity& id) {
    ensure_dir(MOUNTDIR, 0755);
    for (const char* part : {"system", "vendor", "odm", "product", "system_ext"}) {
        std::string dir = std::string(MOUNTDIR) + "/" + part;
        ensure_dir(dir, 0755);
        atomic_write(dir + "/build.prop", build_prop_common(id, part));
    }
    // ---- settings_secure.xml (Android ID + GAID overlay) ----
    auto aid = id.kv.find("ANDROID_ID");
    auto gid = id.kv.find("GAID");
    if (aid != id.kv.end() && gid != id.kv.end()) {
        std::string xml = tt::build_secure_xml(aid->second, gid->second);
        const std::string xml_path = std::string(MOUNTDIR) + "/settings_secure.xml";
        atomic_write(xml_path, xml);
        // v2.1.2: match real Android settings_secure.xml permissions.
        // Owner = system (uid 1000, gid 1000), mode 0600.
        (void)::chmod(xml_path.c_str(), 0600);
        (void)::chown(xml_path.c_str(), 1000, 1000);
    }
}

// -------------------------------------------------------------------------
// Runtime prop injection via resetprop-rs (batched, ARG_MAX-safe)
// -------------------------------------------------------------------------
// v2.0: fork+exec resetprop-rs per property (~200 forks).
// v2.1: single sh -c with all commands concatenated (1 fork total).
// v2.2 P2-C: split into <=24KB batches so we never bump the ARG_MAX ceiling
//            (default 128KB on Android; long fingerprints could hit it).
// v2.2 P3-C: RESETPROP path and prop key are also shell-quoted (defensive:
//            if a future build ever ships a resetprop path with a space or
//            special char, the command still parses correctly).
static void apply_native(const Identity& id) {
    static const std::map<std::string, const char*> propmap = {
        {"BRAND",        "ro.product.brand"},
        {"MANUFACTURER", "ro.product.manufacturer"},
        {"MODEL",        "ro.product.model"},
        {"DEVICE",       "ro.product.device"},
        {"PRODUCT",      "ro.product.name"},
        {"BOARD",        "ro.product.board"},
        {"FINGERPRINT",  "ro.build.fingerprint"},
        {"ID",           "ro.build.id"},
        {"DISPLAY",      "ro.build.display.id"},
        {"RELEASE",      "ro.build.version.release"},
        {"SDK_INT",      "ro.build.version.sdk"},
        {"INCREMENTAL",  "ro.build.version.incremental"},
        {"SECURITY_PATCH","ro.build.version.security_patch"},
        {"HOST",         "ro.build.host"},
        {"USER",         "ro.build.user"},
        {"TYPE",         "ro.build.type"},
        {"TAGS",         "ro.build.tags"},
        {"RADIO",        "gsm.version.baseband"},
    };

    // Shell-escape any string so it fits inside single quotes.
    auto sq = [](std::string_view s) {
        std::string out; out.reserve(s.size() + 2);
        out += '\'';
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += '\'';
        return out;
    };

    // Flush a batched command through a single fork+exec of sh -c.
    auto run_batch = [](const std::string& cmd_str) {
        if (cmd_str.empty()) return;
        pid_t pid = ::fork();
        if (pid < 0) return;
        if (pid == 0) {
            ::execl("/system/bin/sh", "sh", "-c", cmd_str.c_str(), (char*)nullptr);
            ::_exit(127);
        }
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    };

    // v2.2 P2-C: keep each sh -c command under ARG_MAX_SAFE bytes.
    // Real Android ARG_MAX is 128KB, we use 24KB as a very safe ceiling.
    static constexpr std::size_t ARG_MAX_SAFE = 24u * 1024u;
    const std::string rp_q = sq(RESETPROP);   // v2.2 P3-C: quote the path.
    std::string batch;
    batch.reserve(ARG_MAX_SAFE);

    for (const auto& [k, prop] : propmap) {
        auto it = id.kv.find(k);
        if (it == id.kv.end() || it->second.empty()) continue;

        std::string one;
        one.reserve(rp_q.size() + std::strlen(prop) + it->second.size() + 16);
        one.append(rp_q).append(" -n ").append(sq(prop)).append(" ")
           .append(sq(it->second)).append("; ");

        if (!batch.empty() && batch.size() + one.size() > ARG_MAX_SAFE) {
            run_batch(batch);
            batch.clear();
        }
        batch.append(one);
    }
    run_batch(batch);
}

// -------------------------------------------------------------------------
// Public sub-commands (freshen / status / wipe)
// -------------------------------------------------------------------------

static void write_bloom_file() {
    tt::bloom::Filter bf;
    bf.clear();
    std::ifstream f(TARGET_FILE);
    std::string line;
    std::size_t added = 0;
    while (std::getline(f, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        while (!line.empty() &&
               (line.back()=='\n'||line.back()=='\r'||line.back()==' '||line.back()=='\t'))
            line.pop_back();
        std::size_t start = 0;
        while (start < line.size() && (line[start]==' '||line[start]=='\t')) ++start;
        line.erase(0, start);
        if (line.empty()) continue;
        bf.add(line);
        ++added;
    }
    std::string tmp = std::string(BLOOM_FILE) + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    ssize_t left = (ssize_t)sizeof(bf);
    const char* p = reinterpret_cast<const char*>(&bf);
    while (left > 0) {
        ssize_t n = ::write(fd, p, (size_t)left);
        if (n <= 0) { if (errno == EINTR) continue; ::close(fd); ::unlink(tmp.c_str()); return; }
        left -= n; p += n;
    }
    ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), BLOOM_FILE) != 0) { ::unlink(tmp.c_str()); return; }
    std::cout << "ternak-tt: bloom filter written (" << added << " targets, "
              << sizeof(bf) << " bytes)\n";
}

static int cmd_freshen(int argc, char** argv) {
    (void)argc; (void)argv;
    ensure_dir(MODDIR, 0755);

    // Back up previous identity (if any) before overwriting.
    if (file_exists(IDENTITY_FILE)) {
        std::string prev = read_file_or_empty(IDENTITY_FILE);
        atomic_write(IDENTITY_BAK, prev);
    }

    Identity id = gen_identity();
    if (!atomic_write(IDENTITY_FILE, id.dump())) {
        std::cerr << "ternak-tt: failed to write " << IDENTITY_FILE << '\n';
        return 1;
    }
    atomic_write(MODE_FILE, "freshen\n");
    generate_mount_files(id);
    write_bloom_file();
    apply_native(id);

    std::cout << "ternak-tt: freshen ok model=" << id.kv["MODEL"]
              << " brand="      << id.kv["BRAND"]
              << " fp="         << id.kv["FINGERPRINT"] << '\n';
    return 0;
}

static int cmd_status(int, char**) {
    if (!file_exists(IDENTITY_FILE)) {
        std::cout << "ternak-tt: no identity present\n";
        return 1;
    }
    std::cout << read_file_or_empty(IDENTITY_FILE);
    return 0;
}

// Clear app storage for target packages. NO force-stop (v2.0 called
// `am force-stop` which some launchers logged as a suspicious event).
static int cmd_wipe(int argc, char** argv) {
    std::vector<std::string> pkgs;
    for (int i = 2; i < argc; ++i) pkgs.push_back(argv[i]);
    if (pkgs.empty()) {
        std::ifstream f(TARGET_FILE);
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() &&
                   (line.back() == '\n' || line.back() == '\r' ||
                    line.back() == ' '  || line.back() == '\t'))
                line.pop_back();
            if (!line.empty() && line[0] != '#') pkgs.push_back(line);
        }
    }
    for (const auto& p : pkgs) {
        std::string cmd = "pm clear " + p;
        (void)::system(cmd.c_str());
        std::cout << "ternak-tt: wiped " << p << '\n';
    }
    return 0;
}

static int cmd_version(int, char**) {
#ifndef TT_VERSION_STR
#define TT_VERSION_STR "0.0.0-unknown"
#endif
    std::cout << "ternak-tt " << TT_VERSION_STR << '\n';
    return 0;
}

static void print_usage() {
    std::cout <<
        "Usage: ternak-tt <command>\n"
        "  freshen   Generate a new identity and apply it\n"
        "  status    Print current identity.prop contents\n"
        "  wipe [pkg...]  pm clear the target apps (or all in target.txt)\n"
        "  version   Print CLI version\n";
}

int main(int argc, char** argv) {
    if (argc < 2) { print_usage(); return 2; }
    std::string cmd = argv[1];
    if (cmd == "freshen") return cmd_freshen(argc, argv);
    if (cmd == "status")  return cmd_status(argc, argv);
    if (cmd == "wipe")    return cmd_wipe(argc, argv);
    if (cmd == "version") return cmd_version(argc, argv);
    print_usage();
    return 2;
}
