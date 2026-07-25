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
#include <sys/wait.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
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

// v1.0.6: setns(CLONE_NEWNS) requires single-threaded caller (EINVAL otherwise).
// Zygisk-Next companion runtime is multithreaded, so we fork a child. Post-fork
// child is guaranteed single-threaded → setns works. Result piped back to parent.
// Same pattern used by Shamiko / PlayIntegrityFork.
static uint32_t do_mounts_via_fork(uint32_t target_pid) {
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        LOGE("pipe failed: errno=%d", errno);
        return 0;
    }

    pid_t child = ::fork();
    if (child < 0) {
        LOGE("fork failed: errno=%d", errno);
        ::close(pipefd[0]); ::close(pipefd[1]);
        return 0;
    }

    if (child == 0) {
        // Child — single-threaded, so setns(CLONE_NEWNS) works.
        ::close(pipefd[0]);

        char path[64];
        ::snprintf(path, sizeof(path), "/proc/%u/ns/mnt", target_pid);
        int tgt_ns = ::open(path, O_RDONLY | O_CLOEXEC);
        uint32_t ok = 0, fail = 0, skip = 0;

        if (tgt_ns < 0) {
            LOGE("child: open %s failed errno=%d", path, errno);
        } else if (::setns(tgt_ns, CLONE_NEWNS) != 0) {
            LOGE("child: setns->target failed errno=%d", errno);
            ::close(tgt_ns);
        } else {
            for (const auto& e : BIND_ENTRIES) {
                std::string src = std::string(MOUNTDIR) + "/" + e.src_rel;
                if (::access(src.c_str(), F_OK) != 0) { skip++; continue; }
                if (::access(e.dst,        F_OK) != 0) { skip++; continue; }
                if (::mount(src.c_str(), e.dst, nullptr, MS_BIND, nullptr) == 0) {
                    ok++;
                } else {
                    fail++;
                    LOGE("child: bind fail %s -> %s errno=%d",
                         src.c_str(), e.dst, errno);
                }
            }
            LOGI("child mount for pid=%u: %u ok, %u fail, %u skip",
                 target_pid, ok, fail, skip);
            ::close(tgt_ns);
        }

        ::write(pipefd[1], &ok, sizeof(ok));
        ::close(pipefd[1]);
        ::_exit(0);
    }

    // Parent — wait for result
    ::close(pipefd[1]);
    uint32_t ok = 0;
    ssize_t n = ::read(pipefd[0], &ok, sizeof(ok));
    ::close(pipefd[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    if (n != (ssize_t)sizeof(ok)) {
        LOGE("parent: read from child failed (n=%zd)", n);
        return 0;
    }
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
            uint32_t ok = do_mounts_via_fork(pid);
            ::write(client, &ok, sizeof(ok));
        } else {
            break;
        }
    }
    ::close(client);
}
