// jni/companion.cpp - Ternak TT companion process.
//
// Runs OUTSIDE the target app's namespace. Handles two RPCs from the Zygisk
// module (over the companion socket registered with REGISTER_ZYGISK_COMPANION):
//   CMD_GET_IDENTITY - hand back /data/adb/modules/ternak_tt/identity.prop
//                      if the requesting pkg is in target.txt.
//   CMD_DO_MOUNTS    - fork a child, enter the target's mount namespace,
//                      bind-mount ternak_tt/mount/* over the real build.prop
//                      files (and settings_secure.xml).
//
// v1.1.0 refactor:
//   - Removed the duplicate MOUNTDIR / BindEntry / BIND_ENTRIES definitions
//     that used to live at the top of this file. Shared header now.
//   - do_mounts_via_fork() extracted child_do_binds() helper for clarity.

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <time.h>

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <android/log.h>

#include "mount_targets.hpp"

using namespace ternak_tt;

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

static void watch_target_death(uint32_t pid);

enum : uint8_t {
    CMD_CHECK_TT     = 1,
    CMD_GET_IDENTITY = 2,
    CMD_DO_MOUNTS    = 3,
};

// ---- target.txt cache ------------------------------------------------------

static std::vector<std::string> g_targets;
static time_t g_targets_mtime = 0;

static void reload_targets_if_changed() {
    struct stat st{};
    bool have = (::stat(TARGET_FILE, &st) == 0);
    if (!have) {
        if (g_targets.empty()) {
            for (size_t i = 0; i < DEFAULT_TARGETS_COUNT; ++i)
                g_targets.emplace_back(DEFAULT_TARGETS[i]);
            LOGI("target.txt missing, using built-in defaults (%zu pkgs)", g_targets.size());
        }
        return;
    }
    if (!g_targets.empty() && st.st_mtime == g_targets_mtime) return;

    std::ifstream f(TARGET_FILE);
    std::vector<std::string> next;
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
        next.push_back(line);
    }
    if (next.empty()) {
        LOGE("target.txt has 0 valid entries; keeping previous list (%zu pkgs)", g_targets.size());
        g_targets_mtime = st.st_mtime;
        return;
    }
    g_targets = std::move(next);
    g_targets_mtime = st.st_mtime;
    LOGI("target.txt loaded: %zu pkg(s) mtime=%ld", g_targets.size(), (long)st.st_mtime);
#ifdef TT_DEBUG
    for (const auto& p : g_targets) LOGD("  target: %s", p.c_str());
#endif
}

static bool is_target(const std::string& pkg) {
    reload_targets_if_changed();
    for (const auto& t : g_targets) if (t == pkg) return true;
    return false;
}

static std::string read_file(const char* p) {
    std::ifstream f(p);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---- mount pass (fork -> setns -> bind) -----------------------------------

// v1.1.0: extracted from do_mounts_via_fork. Runs INSIDE the forked child
// after it enters the target's mount namespace. Returns (ok, fail, skip)
// via out-parameters. Never longjumps; caller is responsible for _exit().
static void child_do_binds(uint32_t target_pid, uint32_t* ok_out,
                           uint32_t* fail_out, uint32_t* skip_out,
                           uint32_t* skip_src_out, uint32_t* skip_dst_out) {
    uint32_t ok = 0, fail = 0, skip = 0, skip_src = 0, skip_dst = 0;
    LOGD("child: entering %zu-entry bind loop", BIND_ENTRIES_COUNT);
    for (size_t i = 0; i < BIND_ENTRIES_COUNT; ++i) {
        const auto& e = BIND_ENTRIES[i];
        std::string src = std::string(MOUNTDIR) + "/" + e.src_rel;
        bool src_ok = (::access(src.c_str(), F_OK) == 0);
        bool dst_ok = (::access(e.dst,       F_OK) == 0);
        LOGD("  bind check: src=%s(%d) dst=%s(%d)", src.c_str(), src_ok, e.dst, dst_ok);
        if (!src_ok) { ++skip_src; ++skip; continue; }
        if (!dst_ok) { ++skip_dst; ++skip; continue; }
        int rc = ::mount(src.c_str(), e.dst, nullptr, MS_BIND, nullptr);
        if (rc == 0) {
            ++ok;
            LOGD("  bind OK: %s -> %s", src.c_str(), e.dst);
        } else {
            ++fail;
            LOGE("child: bind fail %s -> %s errno=%d", src.c_str(), e.dst, errno);
        }
    }
    LOGI("child mount for pid=%u: %u ok, %u fail, %u skip (skip_src=%u skip_dst=%u) [%s]",
         target_pid, ok, fail, skip, skip_src, skip_dst, TT_VARIANT_TAG);
    *ok_out = ok; *fail_out = fail; *skip_out = skip;
    *skip_src_out = skip_src; *skip_dst_out = skip_dst;
}

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
        // ---- child ----
        ::close(pipefd[0]);
        LOGD("child: pid=%d parent_target=%u", getpid(), target_pid);

        uint32_t ok = 0, fail = 0, skip = 0, skip_src = 0, skip_dst = 0;

        char path[64];
        ::snprintf(path, sizeof(path), "/proc/%u/ns/mnt", target_pid);
        int tgt_ns = ::open(path, O_RDONLY | O_CLOEXEC);
        LOGD("child: open %s -> fd=%d errno=%d", path, tgt_ns, errno);

        if (tgt_ns < 0) {
            LOGE("child: open %s failed errno=%d", path, errno);
        } else if (::setns(tgt_ns, CLONE_NEWNS) != 0) {
            LOGE("child: setns->target failed errno=%d", errno);
            ::close(tgt_ns);
        } else {
            child_do_binds(target_pid, &ok, &fail, &skip, &skip_src, &skip_dst);
            ::close(tgt_ns);
        }

        ::write(pipefd[1], &ok, sizeof(ok));
        ::close(pipefd[1]);
        ::_exit(0);
    }

    // ---- parent ----
    ::close(pipefd[1]);
    uint32_t ok = 0;
    ssize_t n = ::read(pipefd[0], &ok, sizeof(ok));
    ::close(pipefd[0]);

    watch_target_death(target_pid);
    int status = 0;
    ::waitpid(child, &status, 0);
    if (n != (ssize_t)sizeof(ok)) {
        LOGE("parent: read from child failed (n=%zd)", n);
        return 0;
    }
    return ok;
}

static void watch_target_death(uint32_t pid) {
    std::thread([pid]() {
        struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < 3600; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (::kill((pid_t)pid, 0) == 0) continue;
            if (errno != ESRCH) continue;
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

// ---- companion entry point -------------------------------------------------

extern "C" void ternak_tt_companion(int client) {
    LOGD("companion invoked: client=%d pid=%d [%s]", client, getpid(), TT_VARIANT_TAG);
    while (true) {
        uint8_t cmd = 0;
        if (::read(client, &cmd, 1) != 1) break;
        LOGD("recv cmd=%u", cmd);

        if (cmd == CMD_GET_IDENTITY) {
            uint16_t plen = 0;
            if (::read(client, &plen, sizeof(plen)) != (ssize_t)sizeof(plen)) break;
            std::string pkg;
            if (plen) {
                pkg.resize(plen);
                size_t got = 0;
                while (got < plen) {
                    ssize_t n = ::read(client, &pkg[got], plen - got);
                    if (n <= 0) break;
                    got += (size_t)n;
                }
                if (got != plen) break;
            }
            if (!is_target(pkg)) {
                LOGD("REJECT pkg='%s' (not in target.txt)", pkg.c_str());
                uint32_t z = 0;
                ::write(client, &z, sizeof(z));
                continue;
            }
            LOGD("ACCEPT pkg='%s'", pkg.c_str());
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
