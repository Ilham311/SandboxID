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

        if (!api_->exemptFd(fd))
            LOGW("exemptFd(fd=%d) returned false — companion socket may be closed by "
                 "zygote; bind-mount step will be skipped for this process", fd);

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
            request_companion_mounts(comp_fd_);

            if (val("SBX_HIDE") == "1") request_companion_hide(comp_fd_);
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