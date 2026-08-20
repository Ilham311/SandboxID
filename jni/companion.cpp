// ============================================================================
// Ternak TT — Root-side companion: identity reader + bind-mount via setns
// Fix v1.1: seed on-demand, death watcher double-fork, errno hygiene
// ============================================================================
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
#include <array>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <mutex>
#include <time.h>
#include <android/log.h>
#include "tt_config.hpp"

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

// Budget IO: request dari module tidak boleh menggantung > 2 detik.
static constexpr struct timeval TT_IO_TIMEOUT = {2, 0};

// ============================================================================
// Target list management (hot-reload target.txt)
// ============================================================================
static void watch_target_death(uint32_t pid, int client_fd);

static std::vector<std::string> g_targets;
static time_t                   g_targets_mtime = 0;
static std::recursive_mutex     g_targets_mtx;

// Reload target.txt kalau mtime berubah; fallback ke built-in defaults kalau file hilang.
static void reload_targets_if_changed() {
    std::lock_guard<std::recursive_mutex> lock(g_targets_mtx);
    struct stat st{};
    bool have = (::stat(tt::TARGET_FILE, &st) == 0);
    if (!have) {
        if (g_targets.empty()) {
            g_targets = {
                "com.zhiliaoapp.musically",
                "com.ss.android.ugc.trill",
                "com.zhiliaoapp.musically.go",
                "com.grabtaxi.passenger",
            };
            LOGI("target.txt missing, using built-in defaults (%zu pkgs)", g_targets.size());
        }
        return;
    }
    if (!g_targets.empty() && st.st_mtime == g_targets_mtime) return;

    std::ifstream f(tt::TARGET_FILE);
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
    g_targets       = std::move(next);
    g_targets_mtime = st.st_mtime;
    LOGI("target.txt loaded: %zu pkg(s) mtime=%ld", g_targets.size(), (long)st.st_mtime);
#ifdef TT_DEBUG
    for (const auto& p : g_targets) LOGD("  target: %s", p.c_str());
#endif
}

// Cek apakah package ada di target list (dengan hot-reload).
static bool is_target(const std::string& pkg) {
    if (pkg.empty()) return false;
    std::lock_guard<std::recursive_mutex> lock(g_targets_mtx);
    reload_targets_if_changed();
    for (const auto& t : g_targets) if (t == pkg) return true;
    return false;
}

// Baca file ke string; return "" kalau gagal.
static std::string read_file(const char* p) {
    std::ifstream f(p);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ============================================================================
// Mount operations — fork-based untuk single-threaded setns safety
// ============================================================================
struct MountResult {
    uint32_t ok = 0, fail = 0, skip = 0, skip_src = 0, skip_dst = 0;
    int32_t  ns_open_errno   = 0;
    int32_t  setns_errno     = 0;
    int32_t  slave_errno     = 0;
    int32_t  first_fail_idx  = -1;
    int32_t  first_fail_errno= 0;
};

// Fork child untuk setns+bind-mount; child single-threaded supaya setns tidak EINVAL.
// Return jumlah mount yang sukses.
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
        // === CHILD: single-threaded, aman untuk setns ===
        ::close(pipefd[0]);
        MountResult r;

        // Buka semua src fd SEBELUM setns (path source di namespace companion)
        std::array<int, tt::BIND_ENTRIES_N> src_fds{};
        for (size_t i = 0; i < tt::BIND_ENTRIES_N; ++i) {
            std::string src = std::string(tt::MOUNTDIR) + "/" + tt::BIND_ENTRIES[i].src_rel;
            src_fds[i] = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
        }

        // setns ke mount namespace target
        char path[64];
        ::snprintf(path, sizeof(path), "/proc/%u/ns/mnt", target_pid);
        int tgt_ns = ::open(path, O_RDONLY | O_CLOEXEC);
        if (tgt_ns < 0) {
            r.ns_open_errno = errno;
        } else if (::setns(tgt_ns, CLONE_NEWNS) != 0) {
            r.setns_errno = errno;
            ::close(tgt_ns);
        } else {
            // Pastikan mount tidak bocor via propagasi; laporkan kalau gagal.
            if (::mount("", "/", nullptr, MS_SLAVE | MS_REC, nullptr) != 0)
                r.slave_errno = errno;

            for (size_t i = 0; i < tt::BIND_ENTRIES_N; ++i) {
                const auto& e = tt::BIND_ENTRIES[i];
                if (src_fds[i] < 0) { r.skip_src++; r.skip++; continue; }
                if (::access(e.dst, F_OK) != 0) { r.skip_dst++; r.skip++; continue; }

                // Mount via /proc/self/fd supaya path source tidak permanen
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

        // Tutup semua src fd
        for (size_t i = 0; i < tt::BIND_ENTRIES_N; ++i)
            if (src_fds[i] >= 0) ::close(src_fds[i]);

        // Kirim hasil ke parent
        tt::write_full(pipefd[1], &r, sizeof(r));
        ::close(pipefd[1]);
        ::_exit(0);
    }

    // === PARENT: collect result ===
    ::close(pipefd[1]);
    MountResult r;
    bool got = tt::read_full(pipefd[0], &r, sizeof(r));
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
        LOGE("mount pid=%u: MS_SLAVE gagal errno=%d — bind mount bisa bocor via propagasi!",
             target_pid, r.slave_errno);
    }
    if (r.fail && r.first_fail_idx >= 0 && r.first_fail_idx < (int)tt::BIND_ENTRIES_N) {
        LOGE("mount pid=%u: %u bind(s) FAILED (first: %s errno=%d) [%s]",
             target_pid, r.fail, tt::BIND_ENTRIES[r.first_fail_idx].dst,
             r.first_fail_errno, TT_VARIANT_TAG);
    }
    LOGI("mount pid=%u: %u ok, %u fail, %u skip (skip_src=%u skip_dst=%u) [%s]",
         target_pid, r.ok, r.fail, r.skip, r.skip_src, r.skip_dst, TT_VARIANT_TAG);

    // Spawn death watcher SETELAH pipe waitpid selesai (child mount sudah clean)
    watch_target_death(target_pid, client);

    return r.ok;
}

// ============================================================================
// Death watcher — double-fork agar selamat saat handler companion exit
// ============================================================================
// Double-fork: proses watcher terpisah dari handler companion, jadi tetap hidup
// setelah koneksi client ditutup dan handler exit.
static void watch_target_death(uint32_t pid, int client_fd) {
    pid_t f1 = ::fork();
    if (f1 < 0) return;
    if (f1 > 0) return;  // parent (handler companion) lanjut

    // f1 == 0: fork kedua supaya init yang jadi parent (tidak jadi zombie)
    if (::fork() > 0) ::_exit(0);

    // f2 == 0: watcher process — tutup fd client supaya tidak menahan koneksi
    ::close(client_fd);

    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 3600; ++i) {  // 30 menit @ 500ms
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (::kill((pid_t)pid, 0) == 0) continue;
        if (errno != ESRCH) continue;
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        LOGI("DEATH target pid=%u disappeared after %ldms [%s]", pid, ms, TT_VARIANT_TAG);
        ::_exit(0);
    }
    LOGD("death watcher for pid=%u timed out after 30min", pid);
    ::_exit(0);
}

// ============================================================================
// Seed on-demand — exec ternak-tt seed kalau identity.prop kosong
// ============================================================================
// Seed on-demand via exec CLI: menutup race first-spawn tanpa duplikasi generator.
static bool try_seed_ondemand() {
    std::string bin = std::string(tt::MODDIR) + "/bin/ternak-tt";
    if (::access(bin.c_str(), X_OK) != 0) {
        LOGE("seed on-demand: binary not found at %s", bin.c_str());
        return false;
    }

    pid_t s = ::fork();
    if (s < 0) return false;
    if (s == 0) {
        char arg0[] = "ternak-tt";
        char arg1[] = "seed";
        char* const argv[] = {arg0, arg1, nullptr};
        execv(bin.c_str(), argv);
        ::_exit(127);
    }
    int st = 0;
    ::waitpid(s, &st, 0);
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

// ============================================================================
// Companion entry point — dipanggil oleh Zygisk per koneksi
// ============================================================================
extern "C" void ternak_tt_companion(int client) {
    LOGD("companion invoked: client=%d pid=%d [%s]", client, getpid(), TT_VARIANT_TAG);

    // Set timeout supaya handler tidak menggantung selamanya
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &TT_IO_TIMEOUT, sizeof(TT_IO_TIMEOUT));
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &TT_IO_TIMEOUT, sizeof(TT_IO_TIMEOUT));

    while (true) {
        uint8_t cmd = 0;
        if (!tt::read_full(client, &cmd, 1)) break;
        LOGD("recv cmd=%u", cmd);

        if (cmd == tt::CMD_GET_IDENTITY) {
            uint16_t plen = 0;
            if (!tt::read_full(client, &plen, sizeof(plen))) break;
            std::string pkg;
            if (plen) {
                pkg.resize(plen);
                if (!tt::read_full(client, &pkg[0], plen)) break;
            }

            if (!is_target(pkg)) {
                LOGD("REJECT pkg='%s' (not in target.txt)", pkg.c_str());
                uint32_t z = 0;
                tt::write_full(client, &z, sizeof(z));
                continue;
            }

            std::string d = read_file(tt::IDENTITY_FILE);
            if (d.empty()) {
                // Seed on-demand: coba exec ternak-tt seed, lalu baca ulang.
                for (int attempt = 0; attempt < 3 && d.empty(); ++attempt) {
                    if (attempt > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (try_seed_ondemand())
                        d = read_file(tt::IDENTITY_FILE);
                }
                if (d.empty())
                    LOGE("target '%s' tapi identity.prop kosong SETELAH seed on-demand — "
                         "fail-open: app jalan TANPA spoofing", pkg.c_str());
            }
            LOGD("ACCEPT pkg='%s' (%zu bytes)", pkg.c_str(), d.size());

            uint32_t l = (uint32_t)d.size();
            if (!tt::write_full(client, &l, sizeof(l))) break;
            if (l && !tt::write_full(client, d.data(), l)) break;

        } else if (cmd == tt::CMD_DO_MOUNTS) {
            uint32_t pid = 0;
            if (!tt::read_full(client, &pid, sizeof(pid))) break;
            if (pid == 0) {
                uint32_t z = 0;
                tt::write_full(client, &z, sizeof(z));
                break;
            }
            uint32_t ok = do_mounts_via_fork(pid, client);
            if (!tt::write_full(client, &ok, sizeof(ok))) break;

        } else {
            LOGD("unknown cmd=%u, closing", cmd);
            break;
        }
    }
    ::close(client);
}