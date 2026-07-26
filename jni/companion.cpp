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
#include <thread>
#include <chrono>
#include <signal.h>
#include <time.h>
#include <android/log.h>

#define LOG_TAG "TernakTTCompanion"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#ifdef TT_DEBUG
#define LOGD(fmt, ...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[D] " fmt, ##__VA_ARGS__)
#define TT_VARIANT_TAG "debug"
#else
#define LOGD(...) ((void)0)
#define TT_VARIANT_TAG "release"
#endif

// Forward decls
static void watch_target_death(uint32_t pid);

enum : uint8_t {
    CMD_CHECK_TT     = 1,
    CMD_GET_IDENTITY = 2,
    CMD_DO_MOUNTS    = 3,  // v1.0.4: setns into caller mnt ns + bind mount
};

static const char* IDENTITY_FILE = "/data/adb/modules/ternak_tt/identity.prop";
static const char* MOUNTDIR      = "/data/adb/modules/ternak_tt/mount";

struct BindEntry { const char* src_rel; const char* dst; };
// v1.0.14: added alternate destination paths for odm/product/system_ext.
// POCO F3 (MIUI15, tested) uses Android 11+ canonical layout where
// partition build.prop lives at /<part>/etc/build.prop, not /<part>/build.prop.
// v1.0.12 telemetry showed 3 skip on those partitions because the legacy
// dst path didn't exist. Trying both paths per partition; at most one dst
// exists per partition so the alternate is a silent skip on other ROMs.
static const BindEntry BIND_ENTRIES[] = {
    {"system/build.prop",     "/system/build.prop"},
    {"vendor/build.prop",     "/vendor/build.prop"},
    {"odm/build.prop",        "/odm/etc/build.prop"},           // Android 11+
    {"odm/build.prop",        "/odm/build.prop"},               // legacy
    {"product/build.prop",    "/product/etc/build.prop"},       // Android 11+
    {"product/build.prop",    "/product/build.prop"},           // legacy
    {"system_ext/build.prop", "/system_ext/etc/build.prop"},    // Android 11+
    {"system_ext/build.prop", "/system_ext/build.prop"},        // legacy
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
        LOGD("child: pid=%d parent_target=%u", getpid(), target_pid);

        char path[64];
        ::snprintf(path, sizeof(path), "/proc/%u/ns/mnt", target_pid);
        int tgt_ns = ::open(path, O_RDONLY | O_CLOEXEC);
        LOGD("child: open %s -> fd=%d errno=%d", path, tgt_ns, errno);
        uint32_t ok = 0, fail = 0, skip = 0;

        if (tgt_ns < 0) {
            LOGE("child: open %s failed errno=%d", path, errno);
        } else if (::setns(tgt_ns, CLONE_NEWNS) != 0) {
            LOGE("child: setns->target failed errno=%d", errno);
            ::close(tgt_ns);
        } else {
            LOGD("child: setns OK, entering %u bind loop",
                 (unsigned)(sizeof(BIND_ENTRIES)/sizeof(BIND_ENTRIES[0])));
            uint32_t skip_src = 0, skip_dst = 0;
            for (const auto& e : BIND_ENTRIES) {
                std::string src = std::string(MOUNTDIR) + "/" + e.src_rel;
                bool src_ok = (::access(src.c_str(), F_OK) == 0);
                bool dst_ok = (::access(e.dst,        F_OK) == 0);
                LOGD("  bind check: src=%s(%d) dst=%s(%d)",
                     src.c_str(), src_ok, e.dst, dst_ok);
                if (!src_ok) { skip_src++; skip++; continue; }
                if (!dst_ok) { skip_dst++; skip++; continue; }
                int rc = ::mount(src.c_str(), e.dst, nullptr, MS_BIND, nullptr);
                if (rc == 0) {
                    ok++;
                    LOGD("  bind OK: %s -> %s", src.c_str(), e.dst);
                } else {
                    fail++;
                    LOGE("child: bind fail %s -> %s errno=%d",
                         src.c_str(), e.dst, errno);
                }
            }
            LOGI("child mount for pid=%u: %u ok, %u fail, %u skip "
                 "(skip_src=%u skip_dst=%u) [%s]",
                 target_pid, ok, fail, skip, skip_src, skip_dst, TT_VARIANT_TAG);
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
    // Arm death watcher in PARENT (companion daemon) — not in child which
    // _exit()s immediately. Watcher runs regardless of mount outcome so we
    // capture every target exit (crash, SIGKILL, LMK, normal close).
    watch_target_death(target_pid);
    int status = 0;
    ::waitpid(child, &status, 0);
    if (n != (ssize_t)sizeof(ok)) {
        LOGE("parent: read from child failed (n=%zd)", n);
        return 0;
    }
    return ok;
}

// ============================================================
// Death watcher: after a successful mount request we spawn a
// detached thread that polls kill(pid,0) until the target dies.
// This surfaces OOM / LMK / SIGKILL kills that the in-process
// signal handler CANNOT see (SIGKILL is uncatchable).
// ============================================================
static void watch_target_death(uint32_t pid) {
    std::thread([pid]() {
        struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
        // Poll every 500ms, give up after 30 min (target likely user-closed).
        for (int i = 0; i < 3600; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (::kill((pid_t)pid, 0) == 0) continue;         // still alive
            if (errno != ESRCH) continue;                     // EPERM etc — keep polling
            struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
            long ms = (t1.tv_sec - t0.tv_sec) * 1000L +
                      (t1.tv_nsec - t0.tv_nsec) / 1000000L;
            LOGI("DEATH target pid=%u disappeared after %ldms "
                 "(uncatchable exit: SIGKILL / LMK / normal exit) [%s]",
                 pid, ms, TT_VARIANT_TAG);
            return;
        }
        LOGD("death watcher for pid=%u timed out after 30min", pid);
    }).detach();
}

extern "C" void ternak_tt_companion(int client) {
    LOGD("companion invoked: client=%d pid=%d [%s]",
         client, getpid(), TT_VARIANT_TAG);
    while (true) {
        uint8_t cmd = 0;
        if (::read(client, &cmd, 1) != 1) break;
        LOGD("recv cmd=%u", cmd);

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
