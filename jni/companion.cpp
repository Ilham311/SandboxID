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
#include <semaphore.h>

#include <android/log.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "tt_paths.hpp"
#include "companion_hardening.hpp"
#include "multiuser_paths.hpp"
#include "tt_proto.hpp"

#define LOG_TAG "TernakTT-Companion"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using tt::paths::MOUNTDIR;
using tt::paths::TARGET_FILE;
using tt::paths::IDENTITY_FILE;
using tt::paths::BUILD_PROP_ENTRIES;
using tt::paths::BUILD_PROP_ENTRIES_N;

#ifndef __NR_pidfd_open
  #define __NR_pidfd_open 434
#endif

static inline int tt_pidfd_open(pid_t pid, unsigned int flags) {
    return (int)::syscall(__NR_pidfd_open, pid, flags);
}

namespace {

constexpr int MAX_CONCURRENT_MOUNT = 8;
sem_t g_mount_sem;
bool  g_mount_sem_init = false;

std::vector<std::string> g_targets;
std::vector<uint8_t>     g_bin_blob;
uint16_t                 g_nkeys = 0;
std::once_flag           g_load_once;
std::mutex               g_reload_mu;

std::vector<std::string> load_targets_file() {
    std::vector<std::string> out;
    FILE* f = ::fopen(TARGET_FILE, "r");
    if (!f) return out;
    char line[256];
    while (::fgets(line, sizeof(line), f)) {
        std::string s(line);
        auto hash = s.find('#');
        if (hash != std::string::npos) s.erase(hash);
        while (!s.empty() &&
               (s.back()=='\n'||s.back()=='\r'||s.back()=='\t'||s.back()==' ')) s.pop_back();
        size_t start = 0;
        while (start < s.size() && (s[start]==' '||s[start]=='\t')) ++start;
        s.erase(0, start);
        if (!s.empty()) out.push_back(std::move(s));
    }
    ::fclose(f);
    return out;
}

std::vector<uint8_t> build_binary_blob_from_file(uint16_t* nkeys_out) {
    std::vector<uint8_t> out;
    *nkeys_out = 0;
    FILE* f = ::fopen(IDENTITY_FILE, "r");
    if (!f) return out;
    char line[1024];
    out.reserve(4096);
    uint16_t n = 0;
    while (::fgets(line, sizeof(line), f)) {
        std::string_view sv(line);
        while (!sv.empty() &&
               (sv.back()=='\n'||sv.back()=='\r'||sv.back()==' '||sv.back()=='\t')) sv.remove_suffix(1);
        if (sv.empty() || sv.front() == '#') continue;
        auto eq = sv.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view k = sv.substr(0, eq);
        std::string_view v = sv.substr(eq + 1);
        if (k.empty() || k.size() > 65535 || v.size() > 65535) continue;
        tt::proto::BinaryEntry e;
        e.klen = (uint16_t)k.size();
        e.vlen = (uint16_t)v.size();
        size_t base = out.size();
        out.resize(base + sizeof(e) + e.klen + e.vlen);
        std::memcpy(out.data() + base, &e, sizeof(e));
        std::memcpy(out.data() + base + sizeof(e), k.data(), e.klen);
        std::memcpy(out.data() + base + sizeof(e) + e.klen, v.data(), e.vlen);
        ++n;
    }
    ::fclose(f);
    *nkeys_out = n;
    return out;
}

void ensure_loaded() {
    std::call_once(g_load_once, []{
        std::lock_guard<std::mutex> lk(g_reload_mu);
        g_targets  = load_targets_file();
        g_bin_blob = build_binary_blob_from_file(&g_nkeys);
        if (!g_mount_sem_init) {
            sem_init(&g_mount_sem, 0, (unsigned)MAX_CONCURRENT_MOUNT);
            g_mount_sem_init = true;
        }
        LOGI("companion loaded: targets=%zu nkeys=%u blob_bytes=%zu",
             g_targets.size(), (unsigned)g_nkeys, g_bin_blob.size());
    });
}

[[nodiscard]] bool is_target(std::string_view pkg) {
    for (const auto& t : g_targets) if (t == pkg) return true;
    return false;
}

[[nodiscard]] int open_target_mnt_ns(pid_t target_pid) {
    int fd = tt_pidfd_open(target_pid, 0);
    if (fd >= 0) return fd;
    char p[64];
    ::snprintf(p, sizeof(p), "/proc/%d/ns/mnt", (int)target_pid);
    return ::open(p, O_RDONLY | O_CLOEXEC);
}

uint32_t do_bind_mounts_in_child(pid_t target_pid) {
    int ns_fd = open_target_mnt_ns(target_pid);
    if (ns_fd < 0) {
        LOGE("open ns for pid %d: %s", (int)target_pid, strerror(errno));
        return 0;
    }
    if (::setns(ns_fd, CLONE_NEWNS) != 0) {
        LOGE("setns pid=%d: %s", (int)target_pid, strerror(errno));
        ::close(ns_fd);
        return 0;
    }
    ::close(ns_fd);

    uint32_t ok = 0;
    char src[512];
    for (size_t i = 0; i < BUILD_PROP_ENTRIES_N; ++i) {
        const auto& e = BUILD_PROP_ENTRIES[i];
        ::snprintf(src, sizeof(src), "%s/%s", MOUNTDIR, e.src_rel);
        if (::access(src, R_OK) != 0) continue;
        if (::access(e.dst, F_OK) != 0) continue;
        if (::mount(src, e.dst, nullptr, MS_BIND, nullptr) == 0) {
            ++ok;
        } else {
            LOGW("bind %s -> %s FAILED: %s", src, e.dst, strerror(errno));
        }
    }

    ::snprintf(src, sizeof(src), "%s/settings_secure.xml", MOUNTDIR);
    if (::access(src, R_OK) == 0) {
        auto user_targets = tt::build_secure_xml_bind_entries();
        for (const auto& e : user_targets) {
            const std::string& dst = e.dst;
            if (::access(dst.c_str(), F_OK) != 0) continue;
            if (::mount(src, dst.c_str(), nullptr, MS_BIND, nullptr) == 0) {
                ++ok;
            } else {
                LOGW("bind %s -> %s FAILED: %s", src, dst.c_str(), strerror(errno));
            }
        }
    }
    return ok;
}

uint32_t do_bind_mounts_forked(pid_t target_pid) {
    sem_wait(&g_mount_sem);
    pid_t child = ::fork();
    if (child < 0) {
        sem_post(&g_mount_sem);
        LOGE("fork failed: %s", strerror(errno));
        return 0;
    }
    if (child == 0) {
        tt::child_init("tt-mount-child");
        uint32_t ok = do_bind_mounts_in_child(target_pid);
        ::_exit(ok > 254 ? 254 : (int)ok);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    sem_post(&g_mount_sem);
    if (WIFEXITED(status)) return (uint32_t)WEXITSTATUS(status);
    return 0;
}

void handle_init_app(int client) {
    tt::proto::InitAppRequestPayload payload{};
    if (!tt::read_all(client, &payload, sizeof(payload))) return;
    if (payload.pkg_len > 512) return;
    std::string pkg(payload.pkg_len, '\0');
    if (payload.pkg_len && !tt::read_all(client, pkg.data(), payload.pkg_len)) return;

    ensure_loaded();

    tt::proto::InitAppResponse resp{};
    tt::proto::fill_header(resp.hdr, tt::proto::CMD_INIT_APP, 0);

    bool tgt = is_target(pkg);
    resp.is_target = tgt ? (uint16_t)1 : (uint16_t)0;
    uint32_t mount_ok = 0;
    if (tgt) {
        mount_ok = do_bind_mounts_forked((pid_t)payload.pid);
        resp.mount_ok = (uint16_t)mount_ok;
        resp.blob_len = (uint32_t)g_bin_blob.size();
        resp.nkeys    = g_nkeys;
        uint32_t body = (uint32_t)(sizeof(resp) - sizeof(tt::proto::Header)) + resp.blob_len;
        resp.hdr.payload_len = body;
    } else {
        uint32_t body = (uint32_t)(sizeof(resp) - sizeof(tt::proto::Header));
        resp.hdr.payload_len = body;
    }

    (void)tt::write_all(client, &resp, sizeof(resp));
    if (tgt && resp.blob_len) {
        (void)tt::write_all(client, g_bin_blob.data(), g_bin_blob.size());
    }
    LOGI("INIT_APP pkg=%s target=%d mount_ok=%u nkeys=%u",
         pkg.c_str(), (int)tgt, (unsigned)mount_ok, (unsigned)g_nkeys);
}

void reject_bad_header(int client) {
    tt::proto::Header err;
    tt::proto::fill_header(err, (uint8_t)0xFF, 0);
    (void)tt::write_all(client, &err, sizeof(err));
    LOGE("companion: bad header (magic/version mismatch)");
}

}

extern "C" void ternak_tt_companion(int client) {
    tt::proto::Header hdr{};
    if (!tt::read_all(client, &hdr, sizeof(hdr))) return;
    if (hdr.magic[0] != tt::proto::MAGIC0 ||
        hdr.magic[1] != tt::proto::MAGIC1 ||
        hdr.magic[2] != tt::proto::MAGIC2 ||
        hdr.version  != tt::proto::VERSION) {
        reject_bad_header(client);
        return;
    }
    switch (hdr.cmd) {
        case tt::proto::CMD_INIT_APP:
            handle_init_app(client);
            break;
        default:
            LOGE("companion: unknown cmd=%u", (unsigned)hdr.cmd);
            reject_bad_header(client);
            break;
    }
}
