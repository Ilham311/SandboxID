#include "module_impl.hpp"
#include "sbx_lsplant.hpp"
#include "sbx_process_name.hpp"
#include <sys/un.h>          // struct ucred, SO_PEERCRED
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

// The zygote's sanitize_fds() runs between preAppSpecialize and postAppSpecialize
// and closes every fd exemptFd() did not exempt. Once the companion socket is
// reaped, its fd NUMBER is recycled by whatever is allocated next - most often
// liblog's logd socket, which install_crash_watchdog()'s warm-up call opens in
// exactly that window. A recycled socket is indistinguishable from a live
// companion by behaviour alone: recv(MSG_PEEK) returns EAGAIN either way, and
// write() of the 5-byte request header succeeds on both. SO_PEERCRED settles it
// - a recycled fd has a different peer pid, or is not a socket at all and the
// getsockopt fails. The credentials are snapshotted in preAppSpecialize, the
// only point where the socket is provably ours.
static bool fd_is_our_companion(int fd, const struct ucred& expected) {
    if (fd < 0) return false;
    struct ucred cr{};
    socklen_t len = sizeof(cr);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0) return false;
    return cr.pid == expected.pid && cr.uid == expected.uid;
}

static SocketState companion_socket_state(int fd, const struct ucred& expected) {
    if (fd < 0) return SocketState::CLOSED;
    if (!fd_is_our_companion(fd, expected)) return SocketState::CLOSED;
    char c;
    ssize_t r = ::recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r == 1) return SocketState::OPEN;                  // reply already pending
    if (r == 0) return SocketState::CLOSED;                // EOF: socket reaped
    if (errno == EAGAIN || errno == EWOULDBLOCK) return SocketState::OPEN;
    // EBADF / ENOTSOCK / ENOTCONN / EINVAL are permanent, not transient: the
    // socket is gone. Only resource exhaustion could plausibly clear on retry,
    // so that alone stays UNKNOWN. A previous revision classified every other
    // errno as UNKNOWN, which routed the reaped-fd case (ENOTSOCK after reuse,
    // EBADF before it) into a round trip that then logged a companion-blaming
    // "MOUNTS: failed to send request" for a fd that was never a socket.
    if (errno == ENOBUFS || errno == ENOMEM) return SocketState::UNKNOWN;
    return SocketState::CLOSED;
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
        if (pkg.empty() && args && args->app_data_dir) {
            JniString raw(env_, args->app_data_dir);
            if (env_->ExceptionCheck()) env_->ExceptionClear();
            std::string dir = raw ? raw.c_str() : "";
            size_t last_slash = dir.rfind('/');
            if (last_slash != std::string::npos && last_slash + 1 < dir.size()) {
                pkg = dir.substr(last_slash + 1);
            }
        }
        LOGD("preAppSpecialize pkg='%s' pid=%d", pkg.c_str(), getpid());
        if (pkg.empty()) { unload(); return; }

        // Refuse child zygotes (AppZygote, "<pkg>_zygote") outright.
        //
        // An AppZygote is a zygote, not an app: the app's real process is forked
        // from it later and gets its own pre/postAppSpecialize under the plain
        // package name, so nothing is lost by skipping this one. Specializing it
        // is actively fatal - every fork the AppZygote then performs runs
        // ForkCommon -> FileDescriptorTable::Restat, which fstat()s every open fd
        // and aborts the process via ZygoteFailure on failure. The per-fork
        // fds_to_ignore list cannot contain a companion socket we opened here, so
        // the AppZygote dies on its next fork:
        //
        //   Abort message: 'JNI FatalError called: (zygote) Unable to stat 120'
        //   Cmdline: com.shopee.id_zygote
        //   FileDescriptorInfo::CreateFromFd <- RestatInternal <- Restat
        //     <- zygote::ForkCommon <- zygote::forkApp
        //
        // 120 was the fd connectCompanion() had just handed back in that pid.
        //
        // is_child_zygote is an *optional* AppSpecializeArgs field, so it is read
        // only when the provider supplied it; the name convention is the
        // fallback. Both Magisk and ReZygisk populate it from the real JNI
        // parameter, so on those providers the flag is authoritative. See
        // jni/sbx_process_name.hpp for the full reasoning.
        const bool have_flag = args && args->is_child_zygote != nullptr;
        const bool flag_value = have_flag && *args->is_child_zygote == JNI_TRUE;
        if (sbxproc::is_child_zygote(have_flag, flag_value, pkg)) {
            LOGI("skipping child zygote '%s' (flag=%s) - not an app process; "
                 "specializing it would abort it on its next fork",
                 pkg.c_str(), have_flag ? (flag_value ? "true" : "false")
                                        : "absent, matched by name");
            unload();
            return;
        }

        int fd = api_->connectCompanion();
        LOGD("connectCompanion() -> fd=%d", fd);
        if (fd < 0) { unload(); return; }

        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &SBX_IO_TIMEOUT, sizeof(SBX_IO_TIMEOUT));
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &SBX_IO_TIMEOUT, sizeof(SBX_IO_TIMEOUT));

        // Snapshot the peer NOW, while the socket is provably ours. This is the
        // only guaranteed opportunity: sanitize_fds() runs after this returns,
        // and once the fd number is recycled there is no way to tell a live
        // companion socket from the socket that replaced it.
        socklen_t peer_len = sizeof(comp_peer_);
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &comp_peer_, &peer_len) == 0)
            comp_peer_valid_ = true;

        // exemptFd() asks the zygote to keep the companion socket open across
        // specialize. Its bool return is NOT a reliable liveness signal:
        //   - Magisk and ZygiskNext return a real bool, and false genuinely
        //     means "the zygote will close this fd" (ZygiskContext::exempt_fd
        //     in Magisk's native/src/core/zygisk/module.cpp).
        //   - ReZygisk declares the same v4 slot as `void (*)(int)` and never
        //     writes a return value, so on arm64 the module reads leftover
        //     garbage from w0 - false 100% of the time, even though the fd WAS
        //     exempted and survives.
        // Acting on the bool alone would skip the hide pass on ReZygisk when
        // the socket actually survived. Record it, but decide on the socket
        // itself in postAppSpecialize (companion_socket_state()).
        //
        // Reported below only for a target: preAppSpecialize runs for every app
        // the zygote forks, and a WARN on each of the ~750 non-target processes
        // per boot drowns the capture. The bool says nothing about an app that
        // never gets a blob.
        fd_exempted_ = api_->exemptFd(fd);

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
        // Both outcomes are logged (LOGI, so release captures see them) because
        // summarize.sh counts each row. LOGI, not LOGW on the false branch: on
        // ReZygisk the slot is void and false is the normal answer, so a warning
        // there would cry wolf on every target activation. The socket probe in
        // postAppSpecialize is what actually decides; its failure is the WARN.
        if (fd_exempted_)
            LOGI("exemptFd() -> true, companion socket exempted");
        else
            LOGI("exemptFd() -> false, the zygote *may* close the companion "
                 "socket (expected on ReZygisk, whose v4 slot is declared void); "
                 "decided on the socket itself in postAppSpecialize");
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!active_) return;

        parse_blob();
        LOGD("parse_blob: %zu identity keys", g_identity.size());

        install_build_hook(env_);
        install_prop_hook(api_, env_);
        install_leak_sensors(api_, env_);
        install_uptime_hook(api_, env_);
        // Canonical base package name for identity/applog derivation. A child
        // zygote never reaches here (refused in preAppSpecialize), but a
        // sub-process (":push", ":isolated") does, and g_pkg must be the plain
        // package so L3, L9 and rotate_ids.sh all seed from one string.
        g_pkg = sbxproc::canonical_pkg(pkg_);
        install_native_read_hooks(api_);
        install_java_hooks(env_);
#ifdef SBX_DEBUG
        for (auto& kv : g_identity) LOGD("  [id] %s = %s", kv.first.c_str(), kv.second.c_str());
#endif
        install_crash_watchdog(pkg_);

        if (comp_fd_ >= 0) {
            // The only post-specialize companion request left is the optional
            // detach-traces hide pass. Property and Build spoofing are entirely
            // in-process now (per-target): there is no global resetprop and no
            // bind-mount layer, so a target that does not opt into hiding has
            // nothing to ask the companion and just closes the socket.
            if (val("SBX_HIDE") == "1") {
                // Decide on the socket, not on exemptFd()'s return: on ReZygisk
                // that bool is garbage while the fd is fine, and on
                // Magisk/ZygiskNext a genuine false leaves the socket reaped. The
                // peer-credential check inside the probe tells the two apart at
                // the moment it matters, and also catches a recycled fd number.
                const SocketState ss = companion_socket_state(comp_fd_, comp_peer_);
                if (ss != SocketState::CLOSED) {
                    // SO_SNDTIMEO/SO_RCVTIMEO live on the socket object, so a
                    // recycled fd carries none and request_companion_hide() would
                    // otherwise have no deadline. Bound the round trip at 2s.
                    ::setsockopt(comp_fd_, SOL_SOCKET, SO_SNDTIMEO,
                                 &SBX_IO_TIMEOUT, sizeof(SBX_IO_TIMEOUT));
                    ::setsockopt(comp_fd_, SOL_SOCKET, SO_RCVTIMEO,
                                 &SBX_IO_TIMEOUT, sizeof(SBX_IO_TIMEOUT));
                    request_companion_hide(comp_fd_);
                } else {
                    // Socket gone - EOF, a permanent errno, or the fd number was
                    // recycled onto a socket with a different peer. In-process
                    // spoofing (Build.*, SystemProperties, L9 file reads) is
                    // unaffected; only the optional trace-detach pass is lost.
                    LOGW("hide pass skipped for '%s': companion socket not usable "
                         "(probe saw EOF, a permanent errno, or a recycled fd "
                         "whose peer pid is no longer the companion). In-process "
                         "spoofing (Build.*, SystemProperties, L9 file reads) is "
                         "intact; only the optional detach-traces pass is skipped. "
                         "summarize.sh reports a count.", pkg_.c_str());
                }
            }
            // Only close the fd if it is still the socket we opened. After a
            // failed round trip the number may already have been recycled onto an
            // unrelated file or pipe the app just opened, and closing that would
            // corrupt the app's own state.
            if (comp_peer_valid_ && fd_is_our_companion(comp_fd_, comp_peer_))
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
    struct ucred comp_peer_{};
    bool comp_peer_valid_ = false;
    std::vector<uint8_t> blob_;

    // L3: the Java-layer hooks. Values come from two places and must agree with
    // what the shell side already wrote to disk:
    //   * the telephony/DRM identifiers are synthesized from a persona seed, and
    //   * openudid/clientudid/cdid are the SAME AppLog ids that rotate_ids.sh
    //     seeds into the app's SharedPreferences (sbxnr::make_applog_ids), so a
    //     register body rewritten by the wire hook carries exactly the values the
    //     SDK read out of its own cache.
    // Fail-soft: without SBX_ENABLE_LSPLANT this compiles to a no-op, and on a
    // device where LSPlant cannot init it logs once and returns.
    void install_java_hooks(JNIEnv* env) {
        sbxlsp::HookValues hv;
        hv.android_id = val("ANDROID_ID");
        hv.serial     = val("SERIAL");
        hv.wifi_mac   = val("WIFI_MAC");
        hv.op_num     = val("GSM_OPERATOR_NUMERIC");
        hv.op_alpha   = val("GSM_OPERATOR_ALPHA");
        hv.op_iso     = val("GSM_OPERATOR_ISO");
        // g_pkg, not pkg_: the canonical base package. sbx_applog_ids() in
        // module_hooks.cpp seeds from g_pkg, and rotate_ids.sh seeds the app's
        // on-disk AppLog cache from the same base name, so all three must agree.
        // Using the raw pkg_ here made L3 diverge from L9 and from disk for
        // every sub-process (":push", ":isolated") - the hook would hand the SDK
        // an openudid the SDK's own cache and the native read path never
        // produced. (AppZygotes never get this far: preAppSpecialize refuses
        // them, so the "_zygote" case this comment once described cannot arise.)
        hv.seed       = sbxnr::applog_seed(val("FINGERPRINT"), hv.serial,
                                           hv.android_id, g_pkg);

        // WebView UA rewrite. The real segment must be read from the values the
        // ROM actually emits, so it comes through sbx_raw_prop_get: plain
        // __system_property_get in this process returns the *persona's* value
        // for every prop the module spoofs, which made from == to and turned
        // the rewrite into a silent no-op (observed on device: the app kept
        // re-persisting the real model and build id).
        {
            char rel[PROP_VALUE_MAX] = {0}, model[PROP_VALUE_MAX] = {0},
                 bid[PROP_VALUE_MAX] = {0};
            sbx_raw_prop_get("ro.build.version.release", rel);
            sbx_raw_prop_get("ro.product.model", model);
            sbx_raw_prop_get("ro.build.id", bid);
            if (rel[0] && model[0] && bid[0]) {
                hv.ua_from = std::string("Android ") + rel + "; " + model +
                             " Build/" + bid;
                const std::string& prel = val("RELEASE");
                const std::string& pmodel = val("MODEL");
                const std::string& pid = val("ID");
                if (!prel.empty() && !pmodel.empty() && !pid.empty())
                    hv.ua_to = "Android " + prel + "; " + pmodel + " Build/" + pid;
            }
        }

        // OEM detection classes. A Pixel cannot load miui.os.Build, and the
        // app's OEM branches key off exactly that: LX/00kR;->p2() turns
        // "class loads" into the apm field "rom" = "MIUI-" + INCREMENTAL, and
        // LX/0D0h;->LIZ() turns it into "rom_version". Hiding the ro.miui.*
        // props does not close this - both helpers fall back to
        // Runtime.exec("getprop") when SystemProperties comes back empty, and
        // a subprocess inherits none of our hooks. Denying the class keeps the
        // branch unreachable instead.
        //
        // Derived from the persona's own MANUFACTURER, not hardcoded: a real
        // Xiaomi persona must keep miui.os.Build, or the module would make the
        // device it is imitating look less like itself.
        {
            const std::string& man = val("MANUFACTURER");
            const std::string& brd = val("BRAND");
            auto is = [&](const char* s) {
                return man == s || brd == s;
            };
            if (!is("Xiaomi") && !is("Redmi") && !is("POCO"))
                hv.deny_classes.emplace_back("miui.os.Build");
            if (!is("Meizu"))
                hv.deny_classes.emplace_back("com.meizu.flyme.reflect.BuildProxy");
            if (!is("OPPO") && !is("OnePlus") && !is("realme"))
                hv.deny_classes.emplace_back("com.oplus.os.OplusBuild");
        }

        const sbxnr::ApplogIds ids = sbxnr::make_applog_ids(
            hv.seed, sbxnr::applog_epoch_or_default(val("APPLOG_EPOCH")));
        hv.openudid   = ids.openudid;
        hv.clientudid = ids.clientudid;
        hv.cdid       = ids.cdid;
        // GAID: a stable UUID per persona. Same derivation the L3 hooks would
        // otherwise have to invent, kept here so it rotates with the seed.
        hv.gaid       = sbxnr::uuid_from_seed(hv.seed ^ 0x47414944ULL);   // "GAID"

        // Display metrics resolution based on persona model
        {
            int dw = 0, dh = 0, dd = 0;
            float xd = 0.0f, yd = 0.0f;
            const std::string& model = val("MODEL");
            if (model.find("Pixel 9 Pro XL") != std::string::npos) {
                dw = 1344; dh = 2992; dd = 560; xd = 486.0f; yd = 486.0f;
            } else if (model.find("Pixel 9 Pro Fold") != std::string::npos) {
                dw = 1080; dh = 2424; dd = 420; xd = 422.0f; yd = 422.0f;
            } else if (model.find("Pixel 9 Pro") != std::string::npos) {
                dw = 1280; dh = 2856; dd = 560; xd = 495.0f; yd = 495.0f;
            } else if (model.find("Pixel 9") != std::string::npos) {
                dw = 1080; dh = 2424; dd = 420; xd = 422.0f; yd = 422.0f;
            } else if (model.find("Pixel 8 Pro") != std::string::npos) {
                dw = 1344; dh = 2992; dd = 560; xd = 489.0f; yd = 489.0f;
            } else if (model.find("Pixel 8") != std::string::npos) {
                dw = 1080; dh = 2400; dd = 420; xd = 428.0f; yd = 428.0f;
            } else if (model.find("Pixel 7 Pro") != std::string::npos) {
                dw = 1440; dh = 3120; dd = 560; xd = 512.0f; yd = 512.0f;
            } else if (model.find("Pixel 7") != std::string::npos) {
                dw = 1080; dh = 2400; dd = 420; xd = 416.0f; yd = 416.0f;
            } else if (model.find("Pixel 6 Pro") != std::string::npos) {
                dw = 1440; dh = 3120; dd = 560; xd = 512.0f; yd = 512.0f;
            } else if (model.find("Pixel 6") != std::string::npos) {
                dw = 1080; dh = 2400; dd = 420; xd = 411.0f; yd = 411.0f;
            } else {
                dw = 1080; dh = 2400; dd = 420; xd = 420.0f; yd = 420.0f;
            }
            if (!val("LCD_DENSITY").empty()) {
                int d = std::atoi(val("LCD_DENSITY").c_str());
                if (d > 0) dd = d;
            }
            if (!val("SCREEN_WIDTH").empty()) {
                int w = std::atoi(val("SCREEN_WIDTH").c_str());
                if (w > 0) dw = w;
            }
            if (!val("SCREEN_HEIGHT").empty()) {
                int h = std::atoi(val("SCREEN_HEIGHT").c_str());
                if (h > 0) dh = h;
            }
            if (!val("SCREEN_XDPI").empty()) {
                float x = std::strtof(val("SCREEN_XDPI").c_str(), nullptr);
                if (x > 0.0f) xd = x;
            }
            if (!val("SCREEN_YDPI").empty()) {
                float y = std::strtof(val("SCREEN_YDPI").c_str(), nullptr);
                if (y > 0.0f) yd = y;
            }
            hv.display_width   = dw;
            hv.display_height  = dh;
            hv.display_density = dd;
            hv.display_xdpi    = xd;
            hv.display_ydpi    = yd;
        }

        if (sbxlsp::install_all(env, hv))
            LOGI("L3 java hooks active");
        else
            // LOGW, not LOGD: in a release build LOGD compiles to nothing, so
            // the one signal that Java-level spoofing is off (android_id, IMEI,
            // MAC, Widevine, the TikTok register path) would vanish with it.
            // This is the difference between "identity spoofed" and "only the
            // property-based layers ran", and it must be visible in release.
            LOGW("L3 java hooks unavailable (L1/L2/L9 tetap jalan)");
    }

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