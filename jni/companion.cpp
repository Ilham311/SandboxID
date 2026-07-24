// ============================================================
// Ternak TT v1.0 — Root companion
// ============================================================
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <pthread.h>
#include <android/log.h>
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
#include "pool_tt.hpp"

#define LOG_TAG "TernakTTCompanion"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

enum : uint8_t { CMD_CHECK_TT = 1, CMD_GET_IDENTITY = 2 };
enum : uint8_t {
    CLI_FRESHEN    = 10,
    CLI_STATUS     = 11,
    CLI_APPLY_BOOT = 12,
    CLI_LOCK       = 13,
    CLI_UNLOCK     = 14,
    CLI_ROLLBACK   = 15,
};

static const char* MODDIR         = "/data/adb/modules/ternak_tt";
static const char* IDENTITY_FILE  = "/data/adb/modules/ternak_tt/identity.prop";
static const char* IDENTITY_BAK   = "/data/adb/modules/ternak_tt/identity.prop.bak";
static const char* MODE_FILE      = "/data/adb/modules/ternak_tt/identity.mode";
static const char* RESETPROP      = "/data/adb/modules/ternak_tt/bin/resetprop-rs";
static const char* UDS_NAME       = "ternak.tt.ctrl";

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
    std::string s; s.reserve(bytes * 2);
    for (int i = 0; i < bytes * 2; ++i) s.push_back(al[d(gen)]);
    return s;
}

static bool atomic_write(const std::string& p, const std::string& data) {
    std::string tmp = p + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    ssize_t w = ::write(fd, data.data(), data.size());
    ::fsync(fd); ::close(fd);
    if (w != (ssize_t)data.size()) { ::unlink(tmp.c_str()); return false; }
    return ::rename(tmp.c_str(), p.c_str()) == 0;
}

static std::string read_file(const std::string& p) {
    std::ifstream f(p);
    if (!f) return "";
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

static std::string trim(std::string s) {
    while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' ')) s.pop_back();
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
    } else if (pid > 0) waitpid(pid, nullptr, 0);
}

// ---- Identity generation ----
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
    h.insert(20, "-"); h.insert(16, "-"); h.insert(12, "-"); h.insert(8, "-");
    h[14] = '4';
    static const char* v = "89ab";
    std::random_device rd; std::mt19937 g(rd());
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
    id.kv["DISPLAY"] = p.id;

    char desc[512];
    snprintf(desc, sizeof(desc), "%s-user %s %s %s release-keys",
             p.product, p.release, p.id, p.incremental);
    id.kv["DESCRIPTION"] = desc;

    std::time_t now = std::time(nullptr);
    struct tm lt; localtime_r(&now, &lt);
    char date[16]; strftime(date, sizeof(date), "%y%m%d", &lt);
    char rad[128];
    snprintf(rad, sizeof(rad), "g5300q-%s-%s-B-%s", date, date, p.incremental);
    id.kv["RADIO"] = rad;

    id.kv["SERIAL"]     = random_hex(8, true);   // 16 hex upper
    id.kv["ANDROID_ID"] = random_hex(8, false);  // 16 hex lower
    id.kv["GOOGLE_AID"] = uuid_v4();

    return id;
}

// ---- Apply native + wipe ----
static void apply_native(const Identity& id) {
    auto get = [&](const char* k) -> std::string {
        auto it = id.kv.find(k); return it != id.kv.end() ? it->second : "";
    };

    struct Rp { const char* key; std::string val; };
    std::vector<Rp> rp = {
        {"ro.serialno",              get("SERIAL")},
        {"ro.boot.serialno",         get("SERIAL")},
        {"ro.build.display.id",      get("DISPLAY")},
        {"ro.build.description",     get("DESCRIPTION")},
        {"gsm.version.baseband",     get("RADIO")},
        {"ro.build.expect.baseband", get("RADIO")},
    };
    if (::access(RESETPROP, X_OK) == 0) {
        for (const auto& r : rp) {
            if (r.val.empty()) continue;
            run_bin(RESETPROP, {"resetprop-rs", "-n", r.key, r.val.c_str()});
        }
    }

    std::string aid = get("ANDROID_ID");
    if (!aid.empty())
        run_bin("/system/bin/settings",
                {"settings", "put", "secure", "android_id", aid.c_str()});

    std::string model = get("MODEL");
    if (!model.empty()) {
        run_bin("/system/bin/settings",
                {"settings", "put", "global", "device_name", model.c_str()});
    }
}

static void wipe_tt_data() {
    for (const char* pkg : TT_PACKAGES) {
        run_bin("/system/bin/pm", {"pm", "clear", pkg});
        run_bin("/system/bin/am", {"am", "force-stop", pkg});
    }
}

static std::string do_freshen() {
    std::string mode = trim(read_file(MODE_FILE));
    if (mode == "locked")
        return "LOCKED: run `ternak-tt unlock` first\n";

    std::string old = read_file(IDENTITY_FILE);
    if (!old.empty()) atomic_write(IDENTITY_BAK, old);

    Identity id = gen_identity();
    if (!atomic_write(IDENTITY_FILE, id.serialize()))
        return "ERROR: failed to write identity.prop\n";

    apply_native(id);
    wipe_tt_data();

    std::string out = "OK \xe2\x80\x94 fresh TT persona ready\n";
    out += "  MODEL       : " + id.kv["MODEL"]          + "\n";
    out += "  DEVICE      : " + id.kv["DEVICE"]         + "\n";
    out += "  RELEASE     : " + id.kv["RELEASE"]
        + " (SDK " + id.kv["SDK_INT"] + ")\n";
    out += "  FINGERPRINT : " + id.kv["FINGERPRINT"]    + "\n";
    out += "  SERIAL      : " + id.kv["SERIAL"]         + "\n";
    out += "  ANDROID_ID  : " + id.kv["ANDROID_ID"]     + "\n";
    out += "  GAID        : " + id.kv["GOOGLE_AID"]     + "\n";
    out += "  SEC PATCH   : " + id.kv["SECURITY_PATCH"] + "\n";
    out += "  Wiped: musically, trill, musically.go\n";
    return out;
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

// ---- UDS listener ----
static void handle_cli(int c) {
    uint8_t cmd = 0;
    if (::read(c, &cmd, 1) != 1) { ::close(c); return; }
    std::string reply;
    switch (cmd) {
        case CLI_FRESHEN:    reply = do_freshen(); break;
        case CLI_APPLY_BOOT: {
            Identity id = load_identity();
            if (id.kv.empty()) { reply = "ERROR: no identity\n"; break; }
            apply_native(id);
            reply = "OK: native prop re-applied\n";
            break;
        }
        case CLI_STATUS: {
            std::string d = read_file(IDENTITY_FILE);
            reply = d.empty() ? "no identity yet\n" : d;
            break;
        }
        case CLI_LOCK:   atomic_write(MODE_FILE, "locked\n"); reply = "OK: locked\n"; break;
        case CLI_UNLOCK: atomic_write(MODE_FILE, "fresh\n");  reply = "OK: unlocked\n"; break;
        case CLI_ROLLBACK: {
            std::string d = read_file(IDENTITY_BAK);
            if (d.empty()) { reply = "no backup\n"; break; }
            atomic_write(IDENTITY_FILE, d);
            apply_native(load_identity());
            wipe_tt_data();
            reply = "OK: rolled back + wiped\n";
            break;
        }
        default: reply = "unknown cmd\n";
    }
    uint32_t rl = (uint32_t)reply.size();
    ::write(c, &rl, sizeof(rl));
    ::write(c, reply.data(), rl);
    ::close(c);
}

static void* uds_listener(void*) {
    int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return nullptr;
    struct sockaddr_un a{};
    a.sun_family = AF_UNIX; a.sun_path[0] = '\0';
    strncpy(a.sun_path + 1, UDS_NAME, sizeof(a.sun_path) - 2);
    socklen_t al = sizeof(sa_family_t) + 1 + strlen(UDS_NAME);
    if (::bind(s, (struct sockaddr*)&a, al) < 0) { ::close(s); return nullptr; }
    if (::listen(s, 8) < 0) { ::close(s); return nullptr; }
    LOGI("UDS @%s ready", UDS_NAME);
    while (true) {
        int c = ::accept(s, nullptr, nullptr);
        if (c < 0) { if (errno == EINTR) continue; break; }
        handle_cli(c);
    }
    ::close(s);
    return nullptr;
}

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void start_uds() {
    pthread_t t;
    if (pthread_create(&t, nullptr, uds_listener, nullptr) == 0) pthread_detach(t);
}

extern "C" void ternak_tt_companion(int client) {
    pthread_once(&g_once, start_uds);
    while (true) {
        uint8_t cmd = 0;
        if (::read(client, &cmd, 1) != 1) break;
        if (cmd == CMD_GET_IDENTITY) {
            std::string d = read_file(IDENTITY_FILE);
            uint32_t l = (uint32_t)d.size();
            ::write(client, &l, sizeof(l));
            if (l) ::write(client, d.data(), l);
        } else break;
    }
    ::close(client);
}
