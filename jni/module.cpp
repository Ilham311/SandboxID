#include "module_impl.hpp"
#ifndef __NR_memfd_create
# if defined(__aarch64__)
#  define __NR_memfd_create 279
# elif defined(__arm__)
#  define __NR_memfd_create 385
# elif defined(__x86_64__)
#  define __NR_memfd_create 319
# elif defined(__i386__)
#  define __NR_memfd_create 356
# endif
#endif


using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

static constexpr struct timeval SBX_IO_TIMEOUT = {2, 0};

// The state of the companion socket. The zygote closes every fd that
// exemptFd() did not exempt during app specialization; a closed client end then
// reads back EOF immediately. MSG_PEEK consumes nothing, so this is safe to run
// before the real request round trip.
//
// An errno that is neither EAGAIN nor EOF (ENOBUFS/ENOMEM on a 1-byte loopback
// peek is close to impossible, but it is not evidence the socket was closed) is
// reported as UNKNOWN rather than CLOSED: the caller warns about this, and a
// diagnostic that claims the zygote reaped the fd must not be driven by an
// error it did not diagnose. UNKNOWN fails open - the round trip is attempted
// and the real read/write error reports itself.
enum class SocketState { OPEN, CLOSED, UNKNOWN };
static SocketState companion_socket_state(int fd) {
    if (fd < 0) return SocketState::UNKNOWN;
    char c;
    ssize_t r = ::recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r == 1) return SocketState::OPEN;                  // reply already pending
    if (r == 0) return SocketState::CLOSED;                // EOF: socket reaped
    if (errno == EAGAIN || errno == EWOULDBLOCK) return SocketState::OPEN;
    return SocketState::UNKNOWN;                           // transient, not dead
}

std::map<std::string, std::string> g_identity;
std::string g_pkg;

class SandboxID : public zygisk::ModuleBase {
public:
    void onLoad(Api* api, JNIEnv* env) override {
        api_ = api; env_ = env;
        LOGD("onLoad build=%s pid=%d uid=%d", SBX_VARIANT_TAG, getpid(), getuid());
    }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        std::string pkg;
        if (args && args->nice_name) {
            JniString raw(env_, args->nice_name);
            if (env_->ExceptionCheck()) env_->ExceptionClear();
            pkg = raw ? raw.c_str() : "";
        }
        LOGD("preAppSpecialize pkg='%s' pid=%d", pkg.c_str(), getpid());
        if (pkg.empty()) { unload(); return; }

        int fd = api_->connectCompanion();
        LOGD("connectCompanion() -> fd=%d", fd);
        if (fd < 0) { unload(); return; }

        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &SBX_IO_TIMEOUT, sizeof(SBX_IO_TIMEOUT));
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &SBX_IO_TIMEOUT, sizeof(SBX_IO_TIMEOUT));

        // exemptFd() asks the zygote to keep the companion socket open across
        // specialize. Its bool return is NOT a reliable liveness signal:
        //   - Magisk and ZygiskNext return a real bool, and false genuinely
        //     means "the zygote will close this fd" (ZygiskContext::exempt_fd
        //     in Magisk's native/src/core/zygisk/module.cpp).
        //   - ReZygisk declares the same v4 slot as `void (*)(int)` and never
        //     writes a return value, so on arm64 the module reads leftover
        //     garbage from w0 - false 100% of the time, even though the fd WAS
        //     exempted and survives.
        // Acting on the bool alone would skip layer-2 bind-mounts on ReZygisk
        // that would have worked. Record it for the log, but decide on the
        // socket itself in postAppSpecialize (companion_socket_alive()).
        fd_exempted_ = api_->exemptFd(fd);
        if (!fd_exempted_) {
            LOGW("exemptFd(fd=%d) returned false - the zygote *may* close the "
                 "companion socket (a known false negative on ReZygisk, whose "
                 "v4 slot is declared void); re-tested in postAppSpecialize", fd);
        } else {
            // Paired with the false branch above so summarize.sh can count
            // both outcomes. Without this there is no success marker anywhere
            // in the tree, and its "exemptFd() returned true" row was
            // provably zero on every device.
            LOGD("exemptFd(fd=%d) returned true - the companion socket is "
                 "exempted", fd);
        }

        uint8_t cmd   = sandboxid::CMD_GET_IDENTITY;
        uint16_t plen = (uint16_t)pkg.size();
        if (!sandboxid::write_full(fd, &cmd, 1) ||
            !sandboxid::write_full(fd, &plen, sizeof(plen)) ||
            (plen && !sandboxid::write_full(fd, pkg.data(), plen))) {
            ::close(fd); unload(); return;
        }

        uint32_t len = 0;
        if (!sandboxid::read_full(fd, &len, sizeof(len)) || len > sandboxid::MAX_IDENTITY_BLOB) {
            ::close(fd); unload(); return;
        }
        if (len == 0) {
            LOGD("pkg='%s' not a target", pkg.c_str());
            ::close(fd); unload(); return;
        }

        blob_.resize(len);
        if (!sandboxid::read_full(fd, blob_.data(), len)) { ::close(fd); unload(); return; }

        active_  = true;
        pkg_     = pkg;
        comp_fd_ = fd;

        LOGI("target active (%u B) [%s]", len, SBX_VARIANT_TAG);
        LOGD("target pkg='%s'", pkg.c_str());
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!active_) return;

        parse_blob();
        LOGD("parse_blob: %zu identity keys", g_identity.size());

        install_build_hook(env_);
        install_prop_hook(api_, env_);
        install_leak_sensors(api_, env_);
        install_uptime_hook(api_, env_);
        g_pkg = pkg_;
        install_native_read_hooks(api_);
#ifdef SBX_DEBUG
        for (auto& kv : g_identity) LOGD("  [id] %s = %s", kv.first.c_str(), kv.second.c_str());
#endif
        install_crash_watchdog(pkg_);

        if (comp_fd_ >= 0) {
            // Decide on the socket, not on exemptFd()'s return: on ReZygisk that
            // bool is garbage while the fd is fine, and on Magisk/ZygiskNext a
            // genuine false leaves the socket reaped. This probe tells the two
            // apart at the moment it actually matters.
            const SocketState ss = companion_socket_state(comp_fd_);
            if (ss != SocketState::CLOSED) {
                request_companion_mounts(comp_fd_);
                if (val("SBX_HIDE") == "1") request_companion_hide(comp_fd_);
            } else {
                // recv() returned EOF, so the zygote really did close the socket
                // during specialize and the mount round trip would only log a
                // companion-blaming "MOUNTS: failed to send request". Say so
                // accurately instead. In-process spoofing (Build.*,
                // SystemProperties, L9 file reads) is unaffected; only direct
                // on-disk build.prop readers inside the app see real values.
                LOGW("layer-2 bind-mounts skipped for '%s': recv() on the "
                     "companion socket returned EOF, so the zygote closed it "
                     "during specialize (exemptFd() returned false in "
                     "preAppSpecialize - on ReZygisk that return is unreliable, "
                     "but EOF is not). In-process spoofing (Build.*, "
                     "SystemProperties, L9 file reads) is intact; only direct "
                     "on-disk build.prop readers see real values. summarize.sh "
                     "reports a count.",
                     pkg_.c_str());
            }
            ::close(comp_fd_);
            comp_fd_ = -1;
        }
    }

    void preServerSpecialize(ServerSpecializeArgs*) override { unload(); }

private:
    Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
    std::string pkg_;
    bool active_ = false;
    bool fd_exempted_ = false;
    int comp_fd_ = -1;
    std::vector<uint8_t> blob_;

    void unload() {
        if (api_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

    void parse_blob() {
        std::string s(blob_.begin(), blob_.end());
        std::istringstream iss(s);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            while (!k.empty() && (k.back()=='\r' || k.back()==' ' || k.back()=='\t'))
                k.pop_back();
            while (!v.empty() && (v.back()=='\r' || v.back()=='\n' || v.back()==' '))
                v.pop_back();
            if (!k.empty()) g_identity[k] = v;
        }
    }
};

REGISTER_ZYGISK_MODULE(SandboxID)

extern "C" void sandboxid_companion(int client);
REGISTER_ZYGISK_COMPANION(sandboxid_companion)