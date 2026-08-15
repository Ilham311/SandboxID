
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/syscall.h>
#ifndef __NR_pidfd_open
#if defined(__aarch64__) || defined(__arm__) || defined(__x86_64__) || defined(__i386__)
#define __NR_pidfd_open 434
#else
#define __NR_pidfd_open -1
#endif
#endif
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
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

static void watch_target_death(uint32_t pid);

enum : uint8_t {
    CMD_GET_IDENTITY = 2,
    CMD_DO_MOUNTS    = 3,
};

static const char* IDENTITY_FILE = "/data/adb/modules/ternak_tt/identity.prop";
#include <mutex>
static const char* MOUNTDIR      = "/data/adb/modules/ternak_tt/mount";
static const char* TARGET_FILE   = "/data/adb/modules/ternak_tt/target.txt";

static std::vector<std::string> g_targets;
static time_t                   g_targets_mtime = 0;
static std::recursive_mutex g_targets_mtx;

static void reload_targets_if_changed() {
    std::lock_guard<std::recursive_mutex> lock(g_targets_mtx);
    struct stat st{};
    bool have = (::stat(TARGET_FILE, &st) == 0);
    if (!have) {
        if (g_targets.empty()) {
            g_targets = {
                "com.zhiliaoapp.musically",
                "com.ss.android.ugc.trill",
                "com.zhiliaoapp.musically.go",
                "com.grabtaxi.passenger",
            };
            LOGI("target.txt missing, using built-in defaults (%zu pkgs)",
                 g_targets.size());
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
        LOGE("target.txt has 0 valid entries; keeping previous list (%zu pkgs)",
             g_targets.size());
        g_targets_mtime = st.st_mtime;
        return;
    }
    g_targets       = std::move(next);
    g_targets_mtime = st.st_mtime;
    LOGI("target.txt loaded: %zu pkg(s) mtime=%ld",
         g_targets.size(), (long)st.st_mtime);
#ifdef TT_DEBUG
    for (const auto& p : g_targets) LOGD("  target: %s", p.c_str());
#endif
}

static bool is_target(const std::string& pkg) {
    std::lock_guard<std::recursive_mutex> lock(g_targets_mtx);
    reload_targets_if_changed();
    for (const auto& t : g_targets) if (t == pkg) return true;
    return false;
}

struct BindEntry { const char* src_rel; const char* dst; };

static const BindEntry BIND_ENTRIES[] = {
    {"system/build.prop",     "/system/build.prop"},
    {"vendor/build.prop",     "/vendor/build.prop"},
    {"odm/build.prop",        "/odm/etc/build.prop"},
    {"odm/build.prop",        "/odm/build.prop"},
    {"product/build.prop",    "/product/etc/build.prop"},
    {"product/build.prop",    "/product/build.prop"},
    {"system_ext/build.prop", "/system_ext/etc/build.prop"},
    {"system_ext/build.prop", "/system_ext/build.prop"},
    {"settings_secure.xml",   "/data/system/users/0/settings_secure.xml"},
};

static std::string read_file(const char* p) {
    int fd = ::open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";

    std::string out;
    char buf[4096];
    size_t total = 0;
    constexpr size_t MAX_SIZE = 262144; // 256 KB

    while (true) {
        ssize_t got = ::read(fd, buf, sizeof(buf));
        if (got <= 0) break;

        if (total + got > MAX_SIZE) {
            out.append(buf, MAX_SIZE - total);
            LOGE("read_file: %s exceeded 256KB ceiling, truncated.", p);
            break;
        }
        out.append(buf, got);
        total += got;
    }
    ::close(fd);
    return out;
}

// Result the forked child ships back to the parent over a pipe. The child runs
// after fork() in a process that may already have background threads (the death
// reaper), so it must stay strictly async-fork-safe: syscalls + stack buffers
// only, no heap allocation and no liblog calls. All logging happens in the
// parent, from the values reported here.
struct MountReport {
    uint32_t ok;
    uint32_t fail;
    uint32_t skip;
    uint32_t skip_src;
    uint32_t skip_dst;
    int32_t  stage;   // 0 = bind loop ran, 1 = open target ns failed, 2 = setns failed, 3 = already mounted
    int32_t  err;     // errno captured for stage 1/2
    int32_t  mount_errno[16];   // 0 = ok/skipped, else errno
};

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

    constexpr size_t num_entries = sizeof(BIND_ENTRIES)/sizeof(BIND_ENTRIES[0]);

    if (child == 0) {
        // ---- async-fork-safe region: no malloc, no liblog ----
        ::close(pipefd[0]);

        int src_fds[num_entries];
        for (size_t i = 0; i < num_entries; ++i) {
            char src[256];
            ::snprintf(src, sizeof(src), "%s/%s", MOUNTDIR, BIND_ENTRIES[i].src_rel);
            src_fds[i] = ::open(src, O_RDONLY | O_CLOEXEC);
        }

        char path[64];
        ::snprintf(path, sizeof(path), "/proc/%u/ns/mnt", target_pid);
        int tgt_ns = ::open(path, O_RDONLY | O_CLOEXEC);

        MountReport rep;
        ::memset(&rep, 0, sizeof(rep));

        if (tgt_ns < 0) {
            rep.stage = 1; rep.err = errno;
        } else if (::setns(tgt_ns, CLONE_NEWNS) != 0) {
            rep.stage = 2; rep.err = errno;
            ::close(tgt_ns);
        } else {
            // Check if pre-fork overlay is already present
            int mnt_fd = ::open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
            bool already_mounted = false;
            uint32_t pre_mounted_count = 0;
            if (mnt_fd >= 0) {
                char buf[16384];
                size_t carry = 0; // bytes of an unterminated partial line kept from the previous chunk
                for (;;) {
                    ssize_t n = ::read(mnt_fd, buf + carry, sizeof(buf) - 1 - carry);
                    if (n <= 0) break;
                    size_t total = carry + (size_t)n;
                    buf[total] = '\0';

                    char* line = buf;
                    char* last_newline = nullptr;
                    {
                        // Find the last newline in this chunk so we know where the
                        // trailing partial line (if any) begins.
                        char* p = buf + total;
                        while (p > buf) {
                            --p;
                            if (*p == '\n') { last_newline = p; break; }
                        }
                    }

                    char* scan_end = last_newline ? last_newline + 1 : buf;
                    while (line < scan_end && *line) {
                        char* next_line = ::strchr(line, '\n');
                        if (!next_line || next_line >= scan_end) break;
                        *next_line = '\0';
                        next_line++;

                        // Parse mountinfo line
                        // Format: 36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue
                        // Fields are space-separated. 4th field is root (source), 5th is mount point (dest).
                        int field_idx = 1;
                        char* p = line;
                        char* root_src = nullptr;
                        char* mount_dst = nullptr;
                        while (*p) {
                            while (*p == ' ') p++;
                            if (!*p) break;
                            char* field_start = p;
                            while (*p && *p != ' ') p++;
                            if (*p) {
                                *p = '\0';
                                p++;
                            }
                            if (field_idx == 4) root_src = field_start;
                            else if (field_idx == 5) mount_dst = field_start;

                            if (field_idx == 5) break;
                            field_idx++;
                        }

                        if (root_src && mount_dst) {
                            for (size_t i = 0; i < num_entries; ++i) {
                                if (::strcmp(mount_dst, BIND_ENTRIES[i].dst) == 0 ||
                                    ::strcmp(root_src, BIND_ENTRIES[i].dst) == 0) {
                                    pre_mounted_count++;
                                    already_mounted = true;
                                    break;
                                }
                            }
                        }
                        line = next_line;
                    }

                    // Carry over any trailing partial line (no newline yet) to the
                    // front of the buffer for the next read, instead of dropping it.
                    size_t remaining = buf + total - scan_end;
                    if (remaining > 0 && remaining < sizeof(buf) - 1) {
                        ::memmove(buf, scan_end, remaining);
                        carry = remaining;
                    } else {
                        // No newline found in a full buffer, or nothing left: drop
                        // the oversized/partial remainder and resync on next read.
                        carry = 0;
                    }

                    if (sizeof(buf) - 1 - carry == 0) {
                        // Buffer full with no newline; reset to avoid infinite loop.
                        carry = 0;
                    }
                }
                ::close(mnt_fd);
            }

            if (already_mounted) {
                rep.stage = 3;
                rep.ok = pre_mounted_count;
            } else {
                rep.stage = 0;
                for (size_t i = 0; i < num_entries; ++i) {
                    const auto& e = BIND_ENTRIES[i];
                    if (src_fds[i] < 0) { rep.skip_src++; rep.skip++; continue; }
                    if (::access(e.dst, F_OK) != 0) { rep.skip_dst++; rep.skip++; continue; }

                    char proc_fd_path[32];
                    ::snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/self/fd/%d", src_fds[i]);

                    if (::mount(proc_fd_path, e.dst, nullptr, MS_BIND, nullptr) == 0) {
                        rep.ok++;
                    } else {
                        rep.fail++;
                        if (i < 16) rep.mount_errno[i] = errno;
                    }
                }
            }
            ::close(tgt_ns);
        }

        for (size_t i = 0; i < num_entries; ++i) {
            if (src_fds[i] >= 0) ::close(src_fds[i]);
        }

        ::write(pipefd[1], &rep, sizeof(rep));
        ::close(pipefd[1]);
        ::_exit(0);
    }

    ::close(pipefd[1]);
    MountReport rep;
    ::memset(&rep, 0, sizeof(rep));
    ssize_t n = ::read(pipefd[0], &rep, sizeof(rep));
    ::close(pipefd[0]);

    watch_target_death(target_pid);
    int status = 0;
    ::waitpid(child, &status, 0);

    if (n != (ssize_t)sizeof(rep)) {
        LOGE("parent: read from child failed (n=%zd)", n);
        return 0;
    }
    if (rep.stage == 1) {
        LOGE("child: open /proc/%u/ns/mnt failed errno=%d", target_pid, rep.err);
        return 0;
    }
    if (rep.stage == 2) {
        LOGE("child: setns->target failed errno=%d", rep.err);
        return 0;
    }
    if (rep.stage == 3) {
        LOGI("pre-fork overlay already present for pid=%u, skipping runtime bind", target_pid);
        return rep.ok;
    }

    LOGI("child mount for pid=%u: %u ok, %u fail, %u skip "
         "(skip_src=%u skip_dst=%u) [%s]",
         target_pid, rep.ok, rep.fail, rep.skip, rep.skip_src, rep.skip_dst, TT_VARIANT_TAG);

    bool mount_locked = false;
    for (size_t i = 0; i < num_entries && i < 16; ++i) {
        if (rep.mount_errno[i] != 0) {
            LOGE("mount fail idx=%zu dst=%s errno=%d (%s)",
                 i, BIND_ENTRIES[i].dst, rep.mount_errno[i],
                 strerror(rep.mount_errno[i]));
            if (rep.mount_errno[i] == EPERM || rep.mount_errno[i] == EINVAL) {
                mount_locked = true;
            }
        }
    }
    if (mount_locked) {
        LOGE("mount blocked by kernel mount-lock; overlay must be "
             "seeded pre-zygote via post-fs-data.sh");
    }

    return rep.ok;
}

static std::mutex g_reaper_mtx;
static std::vector<uint32_t> g_reaper_pids;
static bool g_reaper_running = false;

static void reaper_thread_func() {
    struct Target {
        uint32_t pid;
        int pidfd;
        long start_ms;
    };
    std::vector<Target> targets;

    auto now_ms = []() -> long {
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
    };

    while (true) {
        {
            std::lock_guard<std::mutex> lock(g_reaper_mtx);
            for (uint32_t p : g_reaper_pids) {
                int pfd = (int)::syscall(__NR_pidfd_open, p, 0);
                targets.push_back({p, pfd, now_ms()});
            }
            g_reaper_pids.clear();
        }

        if (targets.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        std::vector<struct pollfd> pfds;
        for (const auto& t : targets) {
            if (t.pidfd >= 0) {
                pfds.push_back({t.pidfd, POLLIN, 0});
            }
        }

        if (!pfds.empty()) {
            int ret = ::poll(pfds.data(), pfds.size(), 500);
            if (ret > 0) {
                for (size_t i = 0; i < targets.size(); ) {
                    if (targets[i].pidfd >= 0) {
                        bool signaled = false;
                        for (const auto& pfd : pfds) {
                            if (pfd.fd == targets[i].pidfd && (pfd.revents & POLLIN)) {
                                signaled = true;
                                break;
                            }
                        }
                        if (signaled) {
                            long alive = now_ms() - targets[i].start_ms;
                            LOGI("DEATH target pid=%u disappeared after %ldms (pidfd) [%s]",
                                 targets[i].pid, alive, TT_VARIANT_TAG);
                            ::close(targets[i].pidfd);
                            targets.erase(targets.begin() + i);
                            continue;
                        }
                    }
                    ++i;
                }
            }
            for (size_t i = 0; i < targets.size(); ) {
                if (now_ms() - targets[i].start_ms > 1800000L) {
                    LOGD("death watcher for pid=%u timed out after 30min", targets[i].pid);
                    if (targets[i].pidfd >= 0) ::close(targets[i].pidfd);
                    targets.erase(targets.begin() + i);
                    continue;
                }
                ++i;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            for (size_t i = 0; i < targets.size(); ) {
                if (::kill((pid_t)targets[i].pid, 0) != 0 && errno == ESRCH) {
                    long alive = now_ms() - targets[i].start_ms;
                    LOGI("DEATH target pid=%u disappeared after %ldms (kill) [%s]",
                         targets[i].pid, alive, TT_VARIANT_TAG);
                    targets.erase(targets.begin() + i);
                    continue;
                }
                if (now_ms() - targets[i].start_ms > 1800000L) {
                    LOGD("death watcher for pid=%u timed out after 30min", targets[i].pid);
                    targets.erase(targets.begin() + i);
                    continue;
                }
                ++i;
            }
        }
    }
}

static void watch_target_death(uint32_t pid) {
    std::lock_guard<std::mutex> lock(g_reaper_mtx);
    if (!g_reaper_running) {
        g_reaper_running = true;
        std::thread(reaper_thread_func).detach();
    }
    g_reaper_pids.push_back(pid);
}

extern "C" void ternak_tt_companion(int client) {
    LOGD("companion invoked: client=%d pid=%d [%s]",
         client, getpid(), TT_VARIANT_TAG);
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
