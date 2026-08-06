// ternak_tt v2.1 - companion.cpp (patched)
//
// Changes vs v2.0:
//   P1-1: CMD_CHECK_TT (unused, no handler) removed. Only CMD_GET_IDENTITY
//         and CMD_DO_MOUNTS remain.
//   P2-1: uses tt::write_all / tt::read_all from companion_hardening.hpp so
//         short reads / writes on the client socket never truncate the
//         identity blob or ack.
//   P2-2: fork() child immediately calls tt::child_init() (close inherited
//         fds, prctl NO_NEW_PRIVS, umask 022) - closes fd-leak vector.
//   P2-3: XML overlay bind-mount now enumerates per-user directories via
//         multiuser_paths.hpp instead of hard-coding /data/system/users/0.
//   P2-4: pidfd_open used for target death watch when kernel >= 5.3;
//         falls back to /proc polling on older kernels. Removes the 100ms
//         busy-loop from v2.0.
//   P3-1: BIND_ENTRIES table removed - now imported from tt_paths.hpp.
//         (Fixes v2.0 drift where main.cpp/companion.cpp/ternak-tt.cpp had
//         three slightly different tables.)

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

#include <android/log.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "tt_paths.hpp"
#include "companion_hardening.hpp"   // tt::write_all / read_all / child_init
#include "multiuser_paths.hpp"       // tt::enumerate_user_secure_targets

#define LOG_TAG "TernakTT-Companion"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#ifdef TT_DEBUG
#define LOGD(fmt, ...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[D] " fmt, ##__VA_ARGS__)
#else
#define LOGD(...) ((void)0)
#endif

using tt::paths::CMD_GET_IDENTITY;
using tt::paths::CMD_DO_MOUNTS;
using tt::paths::MOUNTDIR;
using tt::paths::TARGET_FILE;
using tt::paths::IDENTITY_FILE;
using tt::paths::BUILD_PROP_ENTRIES;
using tt::paths::BUILD_PROP_ENTRIES_N;

// -------------------------------------------------------------------------
// pidfd_open syscall wrapper (added in Linux 5.3 / API 31 in bionic).
// -------------------------------------------------------------------------
#ifndef __NR_pidfd_open
  #if defined(__aarch64__) || defined(__x86_64__)
    #define __NR_pidfd_open 434
  #elif defined(__arm__) || defined(__i386__)
    #define __NR_pidfd_open 434
  #endif
#endif
static inline int tt_pidfd_open(pid_t pid, unsigned int flags) {
    return (int)::syscall(__NR_pidfd_open, pid, flags);
}

// -------------------------------------------------------------------------
// Target package list (loaded from /data/adb/modules/ternak_tt/target.txt).
// -------------------------------------------------------------------------
static std::vector<std::string> load_targets() {
    std::vector<std::string> out;
    FILE* f = ::fopen(TARGET_FILE, "r");
    if (!f) return out;
    char line[256];
    while (::fgets(line, sizeof(line), f)) {
        // trim leading/trailing whitespace + comments
        std::string s(line);
        auto hash = s.find('#');
        if (hash != std::string::npos) s.erase(hash);
        while (!s.empty() &&
               (s.back() == '\n' || s.back() == '\r' ||
                s.back() == '\t' || s.back() == ' ')) s.pop_back();
        size_t start = 0;
        while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
        s.erase(0, start);
        if (!s.empty()) out.push_back(std::move(s));
    }
    ::fclose(f);
    return out;
}

static bool is_target(const std::string& pkg, const std::vector<std::string>& list) {
    for (const auto& t : list) if (t == pkg) return true;
    return false;
}

static std::vector<uint8_t> read_identity_blob() {
    std::vector<uint8_t> out;
    int fd = ::open(IDENTITY_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return out;
    struct stat st;
    if (::fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < (1<<20)) {
        out.resize((size_t)st.st_size);
        size_t got = 0;
        while (got < out.size()) {
            ssize_t n = ::read(fd, out.data() + got, out.size() - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        if (got != out.size()) out.clear();
    }
    ::close(fd);
    return out;
}

// -------------------------------------------------------------------------
// Command handlers
// -------------------------------------------------------------------------

static void handle_get_identity(int client) {
    uint16_t plen = 0;
    if (!tt::read_all(client, &plen, sizeof(plen))) return;
    if (plen == 0 || plen > 512) {
        uint32_t zero = 0;
        tt::write_all(client, &zero, sizeof(zero));
        return;
    }
    std::string pkg(plen, '\0');
    if (!tt::read_all(client, pkg.data(), plen)) return;

    auto list = load_targets();
    if (!is_target(pkg, list)) {
        LOGD("GET_IDENTITY: pkg='%s' NOT a target, sending len=0", pkg.c_str());
        uint32_t zero = 0;
        tt::write_all(client, &zero, sizeof(zero));
        return;
    }

    auto blob = read_identity_blob();
    if (blob.empty()) {
        LOGE("GET_IDENTITY: identity.prop unreadable");
        uint32_t zero = 0;
        tt::write_all(client, &zero, sizeof(zero));
        return;
    }
    uint32_t len = (uint32_t)blob.size();
    tt::write_all(client, &len, sizeof(len));
    tt::write_all(client, blob.data(), blob.size());
    LOGI("GET_IDENTITY: sent %u bytes to pkg=%s", len, pkg.c_str());
}

// -------------------------------------------------------------------------
// Bind-mount: enter target's mount namespace and overlay the build.prop /
// settings_secure.xml files. Runs in a forked child so that failures don't
// take the companion process down.
// -------------------------------------------------------------------------

static uint32_t do_bind_mounts_in_child(pid_t target_pid) {
    // Enter target mount namespace
    char ns_path[64];
    ::snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/mnt", (int)target_pid);
    int ns_fd = ::open(ns_path, O_RDONLY | O_CLOEXEC);
    if (ns_fd < 0) {
        LOGE("open mnt ns for pid %d failed: %s", (int)target_pid, strerror(errno));
        return 0;
    }
    if (::setns(ns_fd, CLONE_NEWNS) != 0) {
        LOGE("setns failed: %s", strerror(errno));
        ::close(ns_fd);
        return 0;
    }
    ::close(ns_fd);

    uint32_t ok = 0;
    char src[512];

    // ---- build.prop overlays ----
    for (size_t i = 0; i < BUILD_PROP_ENTRIES_N; ++i) {
        const auto& e = BUILD_PROP_ENTRIES[i];
        ::snprintf(src, sizeof(src), "%s/%s", MOUNTDIR, e.src_rel);
        if (::access(src, R_OK) != 0) continue;      // source not generated
        if (::access(e.dst, F_OK) != 0)  continue;   // dest doesn't exist
        if (::mount(src, e.dst, nullptr, MS_BIND, nullptr) == 0) {
            ++ok;
            LOGD("bind %s -> %s", src, e.dst);
        } else {
            LOGD("bind %s -> %s failed: %s", src, e.dst, strerror(errno));
        }
    }

    // ---- settings_secure.xml overlays (per user) ----
    ::snprintf(src, sizeof(src), "%s/settings_secure.xml", MOUNTDIR);
    if (::access(src, R_OK) == 0) {
        auto user_targets = tt::enumerate_user_secure_targets();
        for (const auto& dst : user_targets) {
            if (::access(dst.c_str(), F_OK) != 0) continue;
            if (::mount(src, dst.c_str(), nullptr, MS_BIND, nullptr) == 0) {
                ++ok;
                LOGD("bind %s -> %s", src, dst.c_str());
            } else {
                LOGD("bind %s -> %s failed: %s",
                     src, dst.c_str(), strerror(errno));
            }
        }
    }

    return ok;
}

static void handle_do_mounts(int client) {
    uint32_t pid32 = 0;
    if (!tt::read_all(client, &pid32, sizeof(pid32))) return;
    pid_t target_pid = (pid_t)pid32;

    // Fork a helper child. If setns/mount fails, only the child dies.
    pid_t child = ::fork();
    if (child < 0) {
        LOGE("fork failed: %s", strerror(errno));
        uint32_t zero = 0;
        tt::write_all(client, &zero, sizeof(zero));
        return;
    }
    if (child == 0) {
        // Child: harden and do the work.
        tt::child_init();  // close stray fds, NO_NEW_PRIVS, umask, prctl death
        uint32_t ok = do_bind_mounts_in_child(target_pid);
        // Signal parent via exit code (capped at 254)
        ::_exit(ok > 254 ? 254 : (int)ok);
    }

    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    uint32_t ok = 0;
    if (WIFEXITED(status)) ok = (uint32_t)WEXITSTATUS(status);
    tt::write_all(client, &ok, sizeof(ok));
    LOGI("DO_MOUNTS: target_pid=%d ok=%u", (int)target_pid, ok);

    // ---- Death-watch daemon (fire-and-forget) ----
    // Not strictly necessary but useful for debug: log when target dies.
#ifdef TT_DEBUG
    pid_t watcher = ::fork();
    if (watcher == 0) {
        tt::child_init();
        int pfd = tt_pidfd_open(target_pid, 0);
        if (pfd >= 0) {
            // Block until process exits
            struct pollfd pf { pfd, POLLIN, 0 };
            (void)::poll(&pf, 1, -1);
            ::close(pfd);
        } else {
            // Fallback: poll /proc/<pid> every 500ms (NOT every 100ms
            // like v2.0 did)
            char proc[64];
            ::snprintf(proc, sizeof(proc), "/proc/%d", (int)target_pid);
            while (::access(proc, F_OK) == 0) {
                struct timespec ts { 0, 500 * 1000 * 1000 };
                ::nanosleep(&ts, nullptr);
            }
        }
        LOGD("death-watch: pid %d exited", (int)target_pid);
        ::_exit(0);
    }
#endif
}

// -------------------------------------------------------------------------
// Companion entry point (registered from main.cpp).
// -------------------------------------------------------------------------

extern "C" void ternak_tt_companion(int client) {
    uint8_t cmd = 0;
    if (!tt::read_all(client, &cmd, 1)) {
        LOGD("companion: no command byte");
        return;
    }
    switch (cmd) {
        case CMD_GET_IDENTITY: handle_get_identity(client); break;
        case CMD_DO_MOUNTS:    handle_do_mounts(client);    break;
        default:
            LOGE("companion: unknown cmd=%u", (unsigned)cmd);
            break;
    }
}
