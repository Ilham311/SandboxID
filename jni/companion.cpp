// ============================================================
// Ternak TT v1.0.4 - Zygisk companion (identity reader + mount agent)
//
// v1.0.3 bind mount dari preAppSpecialize gagal EACCES di
// Zygisk-Next fork (HMA-OSS) karena CAP_SYS_ADMIN di-drop.
// v1.0.4: companion (yang masih root+caps) yang lakuin mount.
// Companion setns() masuk mount ns TT, mount, setns() balik.
// Teknik ini dipake Shamiko / PlayIntegrityFork.
// ============================================================
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <android/log.h>

#define LOG_TAG "TernakTTCompanion"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

enum : uint8_t {
    CMD_CHECK_TT     = 1,
    CMD_GET_IDENTITY = 2,
    CMD_DO_MOUNTS    = 3,  // v1.0.4: setns into caller mnt ns + bind mount
};

static const char* IDENTITY_FILE = "/data/adb/modules/ternak_tt/identity.prop";
static const char* MOUNTDIR      = "/data/adb/modules/ternak_tt/mount";

struct BindEntry { const char* src_rel; const char* dst; };
static const BindEntry BIND_ENTRIES[] = {
    {"system/build.prop",     "/system/build.prop"},
    {"vendor/build.prop",     "/vendor/build.prop"},
    {"odm/build.prop",        "/odm/build.prop"},
    {"product/build.prop",    "/product/build.prop"},
    {"system_ext/build.prop", "/system_ext/build.prop"},
    {"settings_secure.xml",   "/data/system/users/0/settings_secure.xml"},
};

static std::string read_file(const char* p) {
    std::ifstream f(p);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// setns into target PID's mount namespace, do bind mounts, setns back.
// Returns number of successful mounts (0..6).
static uint32_t do_mounts_in_ns(uint32_t target_pid) {
    // Save our own mount ns so we can return
    int self_ns = ::open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC);
    if (self_ns < 0) {
        LOGE("open self ns failed: errno=%d", errno);
        return 0;
    }

    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/ns/mnt", target_pid);
    int tgt_ns = ::open(path, O_RDONLY | O_CLOEXEC);
    if (tgt_ns < 0) {
        LOGE("open target ns (%s) failed: errno=%d", path, errno);
        ::close(self_ns);
        return 0;
    }

    // Enter target's mount namespace
    if (::setns(tgt_ns, CLONE_NEWNS) != 0) {
        LOGE("setns->target failed: errno=%d", errno);
        ::close(tgt_ns); ::close(self_ns);
        return 0;
    }

    uint32_t ok = 0, fail = 0, skip = 0;
    for (const auto& e : BIND_ENTRIES) {
        std::string src = std::string(MOUNTDIR) + "/" + e.src_rel;
        if (::access(src.c_str(), F_OK) != 0) { skip++; continue; }
        if (::access(e.dst,        F_OK) != 0) { skip++; continue; }
        if (::mount(src.c_str(), e.dst, nullptr, MS_BIND, nullptr) == 0) {
            ok++;
        } else {
            fail++;
            LOGE("bind fail (in ns): %s -> %s errno=%d",
                 src.c_str(), e.dst, errno);
        }
    }
    LOGI("companion mount for pid=%u: %u ok, %u fail, %u skip",
         target_pid, ok, fail, skip);

    // Return to our own mount namespace
    if (::setns(self_ns, CLONE_NEWNS) != 0) {
        // Not fatal (companion process is short-lived anyway), but log it
        LOGE("setns->self failed: errno=%d", errno);
    }
    ::close(tgt_ns);
    ::close(self_ns);

    return ok;
}

extern "C" void ternak_tt_companion(int client) {
    while (true) {
        uint8_t cmd = 0;
        if (::read(client, &cmd, 1) != 1) break;

        if (cmd == CMD_GET_IDENTITY) {
            std::string d = read_file(IDENTITY_FILE);
            uint32_t l = (uint32_t)d.size();
            ::write(client, &l, sizeof(l));
            if (l) ::write(client, d.data(), l);
        } else if (cmd == CMD_DO_MOUNTS) {
            uint32_t pid = 0;
            if (::read(client, &pid, sizeof(pid)) != (ssize_t)sizeof(pid)) break;
            if (pid == 0) { uint32_t z = 0; ::write(client, &z, sizeof(z)); break; }
            uint32_t ok = do_mounts_in_ns(pid);
            ::write(client, &ok, sizeof(ok));
        } else {
            break;
        }
    }
    ::close(client);
}
