
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_set>
#include <array>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <mutex>
#include <time.h>
#include <android/log.h>
#include "config.hpp"
#include "sbx_mountinfo.hpp"

#define LOG_TAG "SandboxIDCompanion"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifdef SBX_DEBUG
#define LOGD(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "[D] " fmt, ##__VA_ARGS__)
#define SBX_VARIANT_TAG "debug"
#else
#define LOGD(...) ((void)0)
#define SBX_VARIANT_TAG "release"
#endif

static constexpr struct timeval SBX_IO_TIMEOUT = {2, 0};

static void watch_target_death(uint32_t pid, int client_fd);

struct TargetFileStamp {
    dev_t dev = 0;
    ino_t ino = 0;
    off_t size = 0;
    time_t mtime_sec = 0;
    long mtime_nsec = 0;
    time_t ctime_sec = 0;
    long ctime_nsec = 0;
    bool valid = false;
};

static std::vector<std::string> g_targets;
static std::unordered_set<std::string> g_target_set;
static TargetFileStamp g_targets_stamp;
static std::mutex g_targets_mtx;

static bool same_target_stamp(const TargetFileStamp& stamp,
                              const struct stat& st) {
    return stamp.valid && stamp.dev == st.st_dev && stamp.ino == st.st_ino &&
           stamp.size == st.st_size && stamp.mtime_sec == st.st_mtim.tv_sec &&
           stamp.mtime_nsec == st.st_mtim.tv_nsec &&
           stamp.ctime_sec == st.st_ctim.tv_sec &&
           stamp.ctime_nsec == st.st_ctim.tv_nsec;
}

static bool same_file_state(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
           a.st_size == b.st_size && a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
           a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
           a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

static void reload_targets_if_changed_locked() {
    int fd = ::open(sandboxid::TARGET_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;

    struct stat before{};
    if (::fstat(fd, &before) != 0) {
        ::close(fd);
        return;
    }

    if (same_target_stamp(g_targets_stamp, before)) {
        ::close(fd);
        return;
    }

    std::string contents;
    if (before.st_size > 0)
        contents.reserve(static_cast<size_t>(before.st_size));
    char buf[4096];
    bool complete = false;
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            contents.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            complete = true;
            break;
        }
        if (errno == EINTR) continue;
        int read_errno = errno;
        LOGW("target.txt reload aborted after read failure errno=%d", read_errno);
        break;
    }

    struct stat after{};
    bool stable = complete && (::fstat(fd, &after) == 0) &&
                  same_file_state(before, after);
    ::close(fd);
    if (!stable) {
        if (complete)
            LOGW("target.txt changed during reload; keeping previous target set");
        return;
    }

    std::istringstream f(contents);
    std::vector<std::string> next;
    std::unordered_set<std::string> next_set;
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
        if (line.empty() || !next_set.insert(line).second) continue;
        next.push_back(line);
    }

    g_targets = std::move(next);
    g_target_set = std::move(next_set);
    g_targets_stamp = {
        after.st_dev, after.st_ino, after.st_size,
        after.st_mtim.tv_sec, after.st_mtim.tv_nsec,
        after.st_ctim.tv_sec, after.st_ctim.tv_nsec, true,
    };
    LOGI("target.txt loaded: %zu pkg(s) dev=%llu ino=%llu size=%lld "
         "mtime=%ld.%09ld", g_targets.size(),
         static_cast<unsigned long long>(after.st_dev),
         static_cast<unsigned long long>(after.st_ino),
         static_cast<long long>(after.st_size),
         static_cast<long>(after.st_mtim.tv_sec),
         static_cast<long>(after.st_mtim.tv_nsec));
#ifdef SBX_DEBUG
    for (const auto& p : g_targets) LOGD("  target: %s", p.c_str());
#endif
}

static bool is_target(const std::string& pkg) {
    if (pkg.empty()) return false;
    std::lock_guard<std::mutex> lock(g_targets_mtx);
    reload_targets_if_changed_locked();
    return g_target_set.find(pkg) != g_target_set.end();
}

static std::string read_file(const char* p) {
    std::ifstream f(p);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct MountResult {
    uint32_t ok = 0, fail = 0, skip = 0, skip_src = 0, skip_dst = 0;
    int32_t  ns_open_errno   = 0;
    int32_t  setns_errno     = 0;
    int32_t  slave_errno     = 0;
    int32_t  first_fail_idx  = -1;
    int32_t  first_fail_errno= 0;
};

static uint32_t do_mounts_via_fork(uint32_t target_pid, int client) {
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

        ::close(pipefd[0]);
        MountResult r;

        std::array<int, sandboxid::BIND_ENTRIES_N> src_fds{};
        for (size_t i = 0; i < sandboxid::BIND_ENTRIES_N; ++i) {
            std::string src = std::string(sandboxid::MOUNTDIR) + "/" + sandboxid::BIND_ENTRIES[i].src_rel;
            src_fds[i] = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
        }

        char path[64];
        ::snprintf(path, sizeof(path), "/proc/%u/ns/mnt", target_pid);
        int tgt_ns = ::open(path, O_RDONLY | O_CLOEXEC);
        if (tgt_ns < 0) {
            r.ns_open_errno = errno;
        } else if (::setns(tgt_ns, CLONE_NEWNS) != 0) {
            r.setns_errno = errno;
            ::close(tgt_ns);
        } else {

            bool propagation_isolated =
                (::mount("", "/", nullptr, MS_SLAVE | MS_REC, nullptr) == 0);
            if (!propagation_isolated) r.slave_errno = errno;

            for (size_t i = 0; propagation_isolated && i < sandboxid::BIND_ENTRIES_N; ++i) {
                const auto& e = sandboxid::BIND_ENTRIES[i];
                if (src_fds[i] < 0) { r.skip_src++; r.skip++; continue; }
                if (::access(e.dst, F_OK) != 0) { r.skip_dst++; r.skip++; continue; }

                char proc_fd_path[32];
                ::snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/self/fd/%d", src_fds[i]);
                if (::mount(proc_fd_path, e.dst, nullptr, MS_BIND, nullptr) == 0) {
                    r.ok++;
                } else {
                    if (r.first_fail_idx < 0) { r.first_fail_idx = (int)i; r.first_fail_errno = errno; }
                    r.fail++;
                }
            }
            ::close(tgt_ns);
        }

        for (size_t i = 0; i < sandboxid::BIND_ENTRIES_N; ++i)
            if (src_fds[i] >= 0) ::close(src_fds[i]);

        sandboxid::write_full(pipefd[1], &r, sizeof(r));
        ::close(pipefd[1]);
        ::_exit(0);
    }

    ::close(pipefd[1]);
    MountResult r;
    bool got = sandboxid::read_full(pipefd[0], &r, sizeof(r));
    ::close(pipefd[0]);

    int status = 0;
    ::waitpid(child, &status, 0);

    if (!got) {
        LOGE("mount child for pid=%u produced no result (crashed?)", target_pid);
        return 0;
    }
    if (r.ns_open_errno) {
        LOGE("mount pid=%u: open /proc/%u/ns/mnt failed errno=%d", target_pid, target_pid, r.ns_open_errno);
        return 0;
    }
    if (r.setns_errno) {
        LOGE("mount pid=%u: setns failed errno=%d", target_pid, r.setns_errno);
        return 0;
    }
    if (r.slave_errno) {

        LOGW("mount pid=%u: MS_SLAVE gagal errno=%d — bind mount bisa bocor via propagasi!",
             target_pid, r.slave_errno);
    }
    if (r.fail && r.first_fail_idx >= 0 && r.first_fail_idx < (int)sandboxid::BIND_ENTRIES_N) {
        LOGE("mount pid=%u: %u bind(s) FAILED (first: %s errno=%d) [%s]",
             target_pid, r.fail, sandboxid::BIND_ENTRIES[r.first_fail_idx].dst,
             r.first_fail_errno, SBX_VARIANT_TAG);
    }
    LOGI("mount pid=%u: %u ok, %u fail, %u skip (skip_src=%u skip_dst=%u) [%s]",
         target_pid, r.ok, r.fail, r.skip, r.skip_src, r.skip_dst, SBX_VARIANT_TAG);

    watch_target_death(target_pid, client);

    return r.ok;
}

struct HideResult {
    uint32_t detached = 0, fail = 0, candidates = 0;
    int32_t  ns_open_errno    = 0;
    int32_t  setns_errno      = 0;
    int32_t  slave_errno      = 0;
    int32_t  mountinfo_errno  = 0;
    int32_t  first_fail_errno = 0;
};

static uint32_t do_hide_via_fork(uint32_t target_pid) {
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        LOGE("hide: pipe failed errno=%d", errno);
        return 0;
    }

    pid_t child = ::fork();
    if (child < 0) {
        LOGE("hide: fork failed errno=%d", errno);
        ::close(pipefd[0]); ::close(pipefd[1]);
        return 0;
    }

    if (child == 0) {

        ::close(pipefd[0]);
        HideResult r;

        char path[64];
        ::snprintf(path, sizeof(path), "/proc/%u/ns/mnt", target_pid);
        int tgt_ns = ::open(path, O_RDONLY | O_CLOEXEC);
        if (tgt_ns < 0) {
            r.ns_open_errno = errno;
        } else if (::setns(tgt_ns, CLONE_NEWNS) != 0) {
            r.setns_errno = errno;
            ::close(tgt_ns);
        } else {

            if (::mount("", "/", nullptr, MS_SLAVE | MS_REC, nullptr) != 0)
                r.slave_errno = errno;

            std::string mi = read_file("/proc/self/mountinfo");
            if (mi.empty()) {
                r.mountinfo_errno = errno ? errno : ENOENT;
            } else {
                std::vector<std::string> targets = sbxmnt::select_umount_targets(mi);
                r.candidates = (uint32_t)targets.size();

                for (const auto& mp : targets) {
                    if (::umount2(mp.c_str(), MNT_DETACH) == 0) {
                        r.detached++;
                    } else {
                        if (!r.first_fail_errno) r.first_fail_errno = errno;
                        r.fail++;
                    }
                }
            }
            ::close(tgt_ns);
        }

        sandboxid::write_full(pipefd[1], &r, sizeof(r));
        ::close(pipefd[1]);
        ::_exit(0);
    }

    ::close(pipefd[1]);
    HideResult r;
    bool got = sandboxid::read_full(pipefd[0], &r, sizeof(r));
    ::close(pipefd[0]);

    int status = 0;
    ::waitpid(child, &status, 0);

    if (!got) {
        LOGE("hide child for pid=%u produced no result (crashed?)", target_pid);
        return 0;
    }
    if (r.ns_open_errno) {
        LOGE("hide pid=%u: open /proc/%u/ns/mnt failed errno=%d", target_pid, target_pid, r.ns_open_errno);
        return 0;
    }
    if (r.setns_errno) {
        LOGE("hide pid=%u: setns failed errno=%d", target_pid, r.setns_errno);
        return 0;
    }
    if (r.slave_errno) {
        LOGW("hide pid=%u: MS_SLAVE gagal errno=%d — detach bisa bocor ke host ns!",
             target_pid, r.slave_errno);
    }
    if (r.mountinfo_errno) {
        LOGE("hide pid=%u: baca /proc/self/mountinfo gagal errno=%d", target_pid, r.mountinfo_errno);
        return 0;
    }
    if (r.fail) {
        LOGW("hide pid=%u: %u detach GAGAL (first errno=%d) dari %u kandidat [%s]",
             target_pid, r.fail, r.first_fail_errno, r.candidates, SBX_VARIANT_TAG);
    }
    LOGI("hide pid=%u: %u detached, %u fail, %u candidate(s) [%s]",
         target_pid, r.detached, r.fail, r.candidates, SBX_VARIANT_TAG);

    return r.detached;
}

static void watch_target_death(uint32_t pid, int client_fd) {
    pid_t f1 = ::fork();
    if (f1 < 0) return;
    if (f1 > 0) {

        ::waitpid(f1, nullptr, 0);
        return;
    }

    if (::fork() > 0) ::_exit(0);

    ::close(client_fd);

    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 3600; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (::kill((pid_t)pid, 0) == 0) continue;
        if (errno != ESRCH) continue;
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        LOGI("DEATH target pid=%u disappeared after %ldms [%s]", pid, ms, SBX_VARIANT_TAG);
        ::_exit(0);
    }
    LOGD("death watcher for pid=%u timed out after 30min", pid);
    ::_exit(0);
}

static bool try_seed_ondemand() {

    std::string base = std::string(sandboxid::MODDIR) + "/bin";
    std::string bin = base + "/sandboxid";
    if (::access(bin.c_str(), X_OK) != 0) {
#if defined(__aarch64__)
        const char* abi = "arm64";
#elif defined(__arm__)
        const char* abi = "arm";
#elif defined(__x86_64__)
        const char* abi = "x86_64";
#elif defined(__i386__)
        const char* abi = "x86";
#else
        const char* abi = nullptr;
#endif
        if (abi == nullptr) {
            LOGE("seed on-demand: no runnable binary in %s (unknown ABI)", base.c_str());
            return false;
        }
        bin = base + "/sandboxid-" + abi;
        if (::access(bin.c_str(), X_OK) != 0) {
            LOGE("seed on-demand: binary not found at %s or %s",
                 (base + "/sandboxid").c_str(), bin.c_str());
            return false;
        }
    }

    pid_t s = ::fork();
    if (s < 0) return false;
    if (s == 0) {
        char arg0[] = "sandboxid";
        char arg1[] = "seed";
        char* const argv[] = {arg0, arg1, nullptr};
        execv(bin.c_str(), argv);
        ::_exit(127);
    }
    int st = 0;
    ::waitpid(s, &st, 0);
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

extern "C" void sandboxid_companion(int client) {
    LOGD("companion invoked: client=%d pid=%d [%s]", client, getpid(), SBX_VARIANT_TAG);

    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &SBX_IO_TIMEOUT, sizeof(SBX_IO_TIMEOUT));
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &SBX_IO_TIMEOUT, sizeof(SBX_IO_TIMEOUT));

    struct ucred peer{};
    socklen_t peer_len = sizeof(peer);
    bool have_peer = (::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) == 0
                      && peer_len == sizeof(peer));
    if (!have_peer)
        LOGW("SO_PEERCRED failed errno=%d — mount/hide authorization will fail closed", errno);
    else
        LOGD("peer creds pid=%d uid=%d gid=%d", peer.pid, peer.uid, peer.gid);

    while (true) {
        uint8_t cmd = 0;
        if (!sandboxid::read_full(client, &cmd, 1)) break;
        LOGD("recv cmd=%u", cmd);

        if (cmd == sandboxid::CMD_GET_IDENTITY) {
            uint16_t plen = 0;
            if (!sandboxid::read_full(client, &plen, sizeof(plen))) break;
            std::string pkg;
            if (plen) {
                pkg.resize(plen);
                if (!sandboxid::read_full(client, &pkg[0], plen)) break;
            }

            if (!is_target(pkg)) {
                LOGD("REJECT pkg='%s' (not in target.txt)", pkg.c_str());
                uint32_t z = 0;
                sandboxid::write_full(client, &z, sizeof(z));
                continue;
            }

            std::string d = read_file(sandboxid::IDENTITY_FILE);
            if (d.empty()) {

                for (int attempt = 0; attempt < 3 && d.empty(); ++attempt) {
                    if (attempt > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (try_seed_ondemand())
                        d = read_file(sandboxid::IDENTITY_FILE);
                }
                if (d.empty())
                    LOGE("target '%s' tapi identity.prop kosong SETELAH seed on-demand — "
                         "fail-open: app jalan TANPA spoofing", pkg.c_str());
            }

            if (!d.empty()) {
                std::string kill = std::string(sandboxid::MODDIR) + "/no_uptime";
                struct stat kst;
                if (::stat(kill.c_str(), &kst) == 0) {
                    static const std::string sec_key = "UPTIME_SECONDS=";
                    static const std::string human_key = "UPTIME_HUMAN=";
                    for (const auto &kv : {std::make_pair(sec_key, std::string("0")),
                                            std::make_pair(human_key, std::string("0d 0h 0m"))}) {
                        const std::string &key = kv.first;
                        size_t p = 0;
                        while (p <= d.size()) {
                            if (d.compare(p, key.size(), key) == 0) {
                                size_t eol = d.find('\n', p);
                                if (eol == std::string::npos) eol = d.size();
                                d.replace(p, eol - p, key + kv.second);
                                break;
                            }
                            size_t nl = d.find('\n', p);
                            if (nl == std::string::npos) break;
                            p = nl + 1;
                        }
                    }
                    LOGD("no_uptime aktif -> UPTIME_SECONDS/UPTIME_HUMAN dipaksa 0 utk '%s'", pkg.c_str());
                }

                std::string nrkill = std::string(sandboxid::MODDIR) + "/no_native_read";
                struct stat nrst;
                if (::stat(nrkill.c_str(), &nrst) == 0) {

                    if (!d.empty() && d.back() != '\n') d.push_back('\n');
                    d += "SBX_NATIVE_READ=0\n";
                    LOGD("no_native_read aktif -> SBX_NATIVE_READ=0 utk '%s'", pkg.c_str());
                }

                if (::stat(sandboxid::ENABLE_HIDE, &nrst) == 0) {

                    if (!d.empty() && d.back() != '\n') d.push_back('\n');
                    d += "SBX_HIDE=1\n";
                    LOGD("enable_hide aktif -> SBX_HIDE=1 utk '%s'", pkg.c_str());
                }
            }
            LOGD("ACCEPT pkg='%s' (%zu bytes)", pkg.c_str(), d.size());

            uint32_t l = (uint32_t)d.size();
            if (!sandboxid::write_full(client, &l, sizeof(l))) break;
            if (l && !sandboxid::write_full(client, d.data(), l)) break;

        } else if (cmd == sandboxid::CMD_DO_MOUNTS) {
            uint32_t pid = 0;
            if (!sandboxid::read_full(client, &pid, sizeof(pid))) break;
            if (pid == 0) {
                uint32_t z = 0;
                sandboxid::write_full(client, &z, sizeof(z));
                break;
            }

            bool authorized = have_peer && peer.uid == 0 && (pid_t)pid == peer.pid;
            if (!authorized) {
                LOGE("DO_MOUNTS DITOLAK: have_peer=%d peer.uid=%d peer.pid=%d target pid=%u "
                     "[SO_PEERCRED fail-closed]",
                     (int)have_peer, have_peer ? peer.uid : -1,
                     have_peer ? peer.pid : -1, pid);
                uint32_t z = 0;
                sandboxid::write_full(client, &z, sizeof(z));
                break;
            }
            uint32_t ok = do_mounts_via_fork(pid, client);
            if (!sandboxid::write_full(client, &ok, sizeof(ok))) break;

        } else if (cmd == sandboxid::CMD_DO_HIDE) {
            uint32_t pid = 0;
            if (!sandboxid::read_full(client, &pid, sizeof(pid))) break;
            if (pid == 0) {
                uint32_t z = 0;
                sandboxid::write_full(client, &z, sizeof(z));
                break;
            }

            bool authorized = have_peer && peer.uid == 0 && (pid_t)pid == peer.pid;
            if (!authorized) {
                LOGE("DO_HIDE DITOLAK: have_peer=%d peer.uid=%d peer.pid=%d target pid=%u "
                     "[SO_PEERCRED fail-closed]",
                     (int)have_peer, have_peer ? peer.uid : -1,
                     have_peer ? peer.pid : -1, pid);
                uint32_t z = 0;
                sandboxid::write_full(client, &z, sizeof(z));
                break;
            }

            struct stat hst{};
            if (::stat(sandboxid::ENABLE_HIDE, &hst) != 0) {
                LOGD("DO_HIDE: enable_hide hilang -> no-op utk pid=%u", pid);
                uint32_t z = 0;
                sandboxid::write_full(client, &z, sizeof(z));
                continue;
            }

            uint32_t n = do_hide_via_fork(pid);
            if (!sandboxid::write_full(client, &n, sizeof(n))) break;

        } else {
            LOGD("unknown cmd=%u, closing", cmd);
            break;
        }
    }
    ::close(client);
}
