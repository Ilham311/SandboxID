#include <unordered_map>


#include <jni.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <android/log.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "zygisk.hpp"
#include "tt_paths.hpp"
#include "JniStringHelper.hpp"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef __NR_memfd_create
  #if defined(__aarch64__)
    #define __NR_memfd_create 279
  #elif defined(__arm__)
    #define __NR_memfd_create 385
  #elif defined(__x86_64__)
    #define __NR_memfd_create 319
  #elif defined(__i386__)
    #define __NR_memfd_create 356
  #endif
#endif
static inline int tt_memfd_create(const char* name, unsigned flags) {
    return (int)syscall(__NR_memfd_create, name, flags);
}

#define LOG_TAG        "TernakTT"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#ifdef TT_DEBUG
#define LOGD(fmt, ...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[D] " fmt, ##__VA_ARGS__)
#define TT_VARIANT_TAG "debug"
#else
#define LOGD(...) ((void)0)
#define TT_VARIANT_TAG "release"
#endif

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;
using tt::paths::CMD_GET_IDENTITY;
using tt::paths::CMD_DO_MOUNTS;

static std::unordered_map<std::string, std::string> g_id;

static const std::string& val(const std::string& k) {
    static const std::string empty;
    auto it = g_id.find(k);
    if (it != g_id.end() && !it->second.empty()) return it->second;

    static const std::unordered_map<std::string, std::string> defaults = {
        {"SYS_BOOT_COMPLETED",    "1"},
        {"GSM_OPERATOR_NUMERIC",  "51010"},
        {"GSM_OPERATOR_ALPHA",    "Telkomsel"},
        {"GSM_OPERATOR_ISO",      "id"},
        {"BUILD_CHARACTERISTICS", "default"},
        {"PERSIST_TIMEZONE",      "Asia/Jakarta"},
        {"CPU_ABI",               "arm64-v8a"},
        {"CPU_ABI2",              ""},
        {"CPU_ABILIST",           "arm64-v8a,armeabi-v7a,armeabi"},
        {"CPU_ABILIST64",         "arm64-v8a"},
        {"CPU_ABILIST32",         "armeabi-v7a,armeabi"},
        {"DALVIK_HEAPGROWTHLIMIT","256m"},
        {"MEDIACODEC_MIN_RATE",   "8000"},
        {"MEDIACODEC_MAX_RATE",   "192000"},
        {"DEBUG_FORCE_RTL",       "false"},
        {"MULTISIM_CONFIG",       "ss"},
    };
    auto d = defaults.find(k);
    if (d != defaults.end()) return d->second;
    return empty;
}

static jstring hook_prop_get(JNIEnv* env, jclass, jstring j_key, jstring j_def) {
    if (!j_key) return j_def;
    tt::JniStringGuard k_guard(env, j_key);
    std::string k = k_guard.str();
    LOGD("L2 native_get('%s') requested", k.c_str());

    static const std::unordered_map<std::string, std::string> map = {
        {"ro.serialno",                    "SERIAL"},
        {"ro.boot.serialno",               "SERIAL"},
        {"ro.build.fingerprint",           "FINGERPRINT"},
        {"ro.bootimage.build.fingerprint", "FINGERPRINT"},
        {"ro.product.model",               "MODEL"},
        {"ro.product.brand",               "BRAND"},
        {"ro.product.manufacturer",        "MANUFACTURER"},
        {"ro.product.device",              "DEVICE"},
        {"ro.product.name",                "PRODUCT"},
        {"ro.product.board",               "BOARD"},
        {"ro.build.id",                    "ID"},
        {"ro.build.display.id",            "DISPLAY"},
        {"ro.build.description",           "DESCRIPTION"},
        {"ro.build.version.release",       "RELEASE"},
        {"ro.build.version.sdk",           "SDK_INT"},
        {"ro.build.version.security_patch","SECURITY_PATCH"},
        {"ro.build.version.incremental",   "INCREMENTAL"},
        {"gsm.version.baseband",           "RADIO"},

        {"sys.boot_completed",             "SYS_BOOT_COMPLETED"},
        {"debug.force_rtl",                "DEBUG_FORCE_RTL"},
        {"persist.radio.multisim.config",  "MULTISIM_CONFIG"},
        {"gsm.operator.numeric",           "GSM_OPERATOR_NUMERIC"},
        {"gsm.sim.operator.numeric",       "GSM_OPERATOR_NUMERIC"},
        {"gsm.operator.alpha",             "GSM_OPERATOR_ALPHA"},
        {"gsm.sim.operator.alpha",         "GSM_OPERATOR_ALPHA"},
        {"gsm.operator.iso-country",       "GSM_OPERATOR_ISO"},
        {"gsm.sim.operator.iso-country",   "GSM_OPERATOR_ISO"},
        {"ro.build.characteristics",       "BUILD_CHARACTERISTICS"},
        {"persist.sys.timezone",           "PERSIST_TIMEZONE"},
        {"ro.product.cpu.abi",             "CPU_ABI"},
        {"ro.product.cpu.abi2",            "CPU_ABI2"},
        {"ro.product.cpu.abilist",         "CPU_ABILIST"},
        {"ro.product.cpu.abilist64",       "CPU_ABILIST64"},
        {"ro.product.cpu.abilist32",       "CPU_ABILIST32"},
        {"dalvik.vm.heapgrowthlimit",      "DALVIK_HEAPGROWTHLIMIT"},
        {"ro.mediacodec.min_sample_rate",  "MEDIACODEC_MIN_RATE"},
        {"ro.mediacodec.max_sample_rate",  "MEDIACODEC_MAX_RATE"},
        {"ro.build.user",                  "USER"},
        {"ro.build.host",                  "HOST"},
        {"ro.build.tags",                  "TAGS"},
        {"ro.build.type",                  "TYPE"},
    };

    static const std::unordered_map<std::string, std::string> static_defaults = {
        {"gsm.operator.isroaming",          "false"},
        {"ro.zygote",                       "zygote64_32"},
        {"ro.hardware",                     "qcom"},
        {"ro.board.platform",               "sm8250"},
        {"ro.dalvik.vm.native.bridge",      "0"},
        {"ro.allow.mock.location",          "0"},
        {"dalvik.vm.isa.arm64.variant",     "generic"},
        {"dalvik.vm.isa.arm64.features",    "default"},
        {"dalvik.vm.isa.arm.variant",       "generic"},
        {"dalvik.vm.isa.arm.features",      "default"},
        {"dalvik.vm.heapsize",              "512m"},
        {"ro.build.version.preview_sdk",    "0"},
        {"telephony.active_modems.max_count","1"},
    };

    auto it = map.find(k);
    if (it != map.end()) {
        const std::string& v = val(it->second);
        if (!v.empty()) {
            LOGD("L2 SPOOF '%s' -> '%s'", k.c_str(), v.c_str());
            return env->NewStringUTF(v.c_str());
        }
    }
    auto sit = static_defaults.find(k);
    if (sit != static_defaults.end()) {
        LOGD("L2 SPOOF-STATIC '%s' -> '%s'", k.c_str(), sit->second.c_str());
        return env->NewStringUTF(sit->second.c_str());
    }

    LOGD("L2 MISS-STRICT '%s' -> java default (no leak)", k.c_str());
    return j_def;
}

static const std::unordered_map<std::string, jboolean>& tt_bool_spoof() {
    static const std::unordered_map<std::string, jboolean> m = {
        {"sys.boot_completed",                     JNI_TRUE},
        {"debug.force_rtl",                        JNI_FALSE},
        {"framework.pause_bg_animations.enabled",  JNI_FALSE},
        {"dalvik.vm.dexopt.secondary",             JNI_TRUE},
        {"viewroot.profile_rendering",             JNI_FALSE},
        {"debug.sqlite.no_double_quoted_strs",     JNI_TRUE},
        {"persist.sys.activity_anim_perf_override",JNI_FALSE},
        {"persist.sys.lmk.reportkills",            JNI_FALSE},
        {"debug.layout",                           JNI_FALSE},
    };
    return m;
}
static const std::unordered_map<std::string, jint>& tt_int_spoof() {
    static const std::unordered_map<std::string, jint> m = {
        {"ro.mediacodec.min_sample_rate",        8000},
        {"ro.mediacodec.max_sample_rate",        192000},
        {"debug.sqlite.wal.autocheckpoint",      100},
        {"debug.sqlite.pagesize",                4096},
        {"debug.sqlite.journalsizelimit",        524288},
        {"debug.sqlite.wal.truncatesize",        1048576},
        {"debug.sqlite.wal.poolsize",            0},
        {"debug.hwui.fps_divisor",               1},
        {"persist.wm.debug.ext_version_override",0},
        {"build.version.extensions.r",           3},
        {"build.version.extensions.s",           4},
        {"build.version.extensions.t",           4},
        {"build.version.extensions.u",           13},
        {"build.version.extensions.v",           13},
        {"build.version.extensions.ad_services", 15},
        {"debug.am.run_gc_trim_level",           2147483647},
        {"debug.am.run_mallopt_trim_level",      2147483647},
        {"debug.adservices.binder_timeout",      10000},
    };
    return m;
}
static const std::unordered_map<std::string, jlong>& tt_long_spoof() {
    static const std::unordered_map<std::string, jlong> m = {
        {"ro.gfx.driver_build_time", 1704067200LL},
    };
    return m;
}

static bool tt_should_suppress_key(const std::string& k) {

    if (k.size() >= 11 + 5 &&
        k.compare(0, 11, "log.looper.") == 0 &&
        k.compare(k.size() - 5, 5, ".slow") == 0) {
        return true;
    }
    if (k.compare(0, 13, "debug.watson.") == 0) return true;
    return false;
}

static bool tt_lookup_int(const std::string& k, jint* out) {
    const auto& m = tt_int_spoof();
    auto it = m.find(k);
    if (it != m.end()) { *out = it->second; return true; }
    if (k == "ro.build.version.sdk") {
        const std::string& s = val("SDK_INT");
        if (!s.empty()) {
            int v = std::atoi(s.c_str());
            if (v > 0) { *out = v; return true; }
        }
    }
    return false;
}

static jint hook_prop_get_int(JNIEnv* env, jclass, jstring j_key, jint def) {
    if (!j_key) return def;
    tt::JniStringGuard k_guard(env, j_key);
    std::string k = k_guard.str();
    jint out = def;
    if (tt_lookup_int(k, &out)) {
        LOGD("L7 SPI '%s' def=%d -> %d [SPOOF]", k.c_str(), def, out);
        return out;
    }
    if (tt_should_suppress_key(k)) {
        LOGD("L7 SPI '%s' def=%d -> %d [SUPPRESS]", k.c_str(), def, def);
        return def;
    }
    LOGD("L7 SPI '%s' def=%d -> %d [LEAK-DEF]", k.c_str(), def, def);
    return def;
}
static jlong hook_prop_get_long(JNIEnv* env, jclass, jstring j_key, jlong def) {
    if (!j_key) return def;
    tt::JniStringGuard k_guard(env, j_key);
    std::string k = k_guard.str();
    const auto& m = tt_long_spoof();
    auto it = m.find(k);
    if (it != m.end()) {
        LOGD("L7 SPL '%s' def=%lld -> %lld [SPOOF]", k.c_str(), (long long)def, (long long)it->second);
        return it->second;
    }
    LOGD("L7 SPL '%s' def=%lld -> %lld", k.c_str(), (long long)def, (long long)def);
    return def;
}
static jboolean hook_prop_get_bool(JNIEnv* env, jclass, jstring j_key, jboolean def) {
    if (!j_key) return def;
    tt::JniStringGuard k_guard(env, j_key);
    std::string k = k_guard.str();
    const auto& m = tt_bool_spoof();
    auto it = m.find(k);
    if (it != m.end()) {
        LOGD("L7 SPB '%s' def=%d -> %d [SPOOF]", k.c_str(), (int)def, (int)it->second);
        return it->second;
    }
    return def;
}

static jstring hook_build_radio(JNIEnv* env, jclass) {
    const std::string& v = val("RADIO");
    if (!v.empty()) return env->NewStringUTF(v.c_str());
    return env->NewStringUTF("");
}

static void set_str(JNIEnv* env, jclass c, const char* f, const std::string& v) {
    if (v.empty()) return;
    jfieldID id = env->GetStaticFieldID(c, f, "Ljava/lang/String;");
    if (!id) { env->ExceptionClear(); return; }
    jstring j = env->NewStringUTF(v.c_str());
    env->SetStaticObjectField(c, id, j);
    env->DeleteLocalRef(j);
}
static void set_int(JNIEnv* env, jclass c, const char* f, int v) {
    jfieldID id = env->GetStaticFieldID(c, f, "I");
    if (!id) { env->ExceptionClear(); return; }
    env->SetStaticIntField(c, id, v);
}

static void install_build_hook(JNIEnv* env) {
    jclass build = env->FindClass("android/os/Build");
    if (build) {
        static const std::vector<std::pair<const char*, const char*>> f = {
            {"BRAND","BRAND"}, {"MANUFACTURER","MANUFACTURER"},
            {"MODEL","MODEL"}, {"DEVICE","DEVICE"}, {"PRODUCT","PRODUCT"},
            {"BOARD","BOARD"}, {"HARDWARE","HARDWARE"},
            {"FINGERPRINT","FINGERPRINT"}, {"ID","ID"},
            {"DISPLAY","DISPLAY"}, {"BOOTLOADER","BOOTLOADER"},
            {"HOST","HOST"}, {"USER","USER"}, {"TYPE","TYPE"},
            {"TAGS","TAGS"}, {"SERIAL","SERIAL"}, {"RADIO","RADIO"},
        };
        for (const auto& [fn, k] : f) set_str(env, build, fn, val(k));
        env->DeleteLocalRef(build);
    } else env->ExceptionClear();

    jclass ver = env->FindClass("android/os/Build$VERSION");
    if (ver) {
        set_str(env, ver, "RELEASE",        val("RELEASE"));
        set_str(env, ver, "INCREMENTAL",    val("INCREMENTAL"));
        set_str(env, ver, "SECURITY_PATCH", val("SECURITY_PATCH"));
        const std::string& s = val("SDK_INT");
        if (!s.empty()) {
            int sdk = std::atoi(s.c_str());
            if (sdk > 0) set_int(env, ver, "SDK_INT", sdk);
        }
        env->DeleteLocalRef(ver);
    } else env->ExceptionClear();
}

static void request_companion_mounts(zygisk::Api* api) {
    if (!api) return;
    int fd = api->connectCompanion();
    if (fd < 0) { LOGE("companion connect for mounts failed"); return; }

    uint8_t cmd = CMD_DO_MOUNTS;
    uint32_t pid = (uint32_t)::getpid();
    if (::write(fd, &cmd, 1) != 1 ||
        ::write(fd, &pid, sizeof(pid)) != (ssize_t)sizeof(pid)) {
        LOGE("companion write failed");
        ::close(fd); return;
    }
    uint32_t ok = 0;
    ssize_t n = ::read(fd, &ok, sizeof(ok));
    ::close(fd);
    if (n != (ssize_t)sizeof(ok)) { LOGE("companion read ack failed"); return; }
    LOGI("bind-mount via companion: %u ok (pid=%u)", ok, pid);
}

using openat_t = int (*)(int, const char*, int, ...);
static openat_t orig_openat = nullptr;

static bool is_sensitive_proc_path(const char* p) {
    if (!p) return false;
    if (::strncmp(p, "/proc/", 6) != 0) return false;

    size_t len = ::strlen(p);
    auto ends_with = [&](const char* suf) {
        size_t sl = ::strlen(suf);
        return len >= sl && ::strcmp(p + len - sl, suf) == 0;
    };
    return ends_with("/mountinfo") ||
           ends_with("/mounts")    ||
           ends_with("/maps")      ||
           ends_with("/status")    ||
           ends_with("/cgroup");
}

static thread_local bool tt_in_openat_hook = false;

static int hook_openat(int dirfd, const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (!orig_openat) return ::openat(dirfd, path, flags, mode);
    if (tt_in_openat_hook || !is_sensitive_proc_path(path)) {
        return orig_openat(dirfd, path, flags, mode);
    }

    tt_in_openat_hook = true;
    int real_fd = orig_openat(dirfd, path, flags, mode);
    if (real_fd < 0) { tt_in_openat_hook = false; return real_fd; }

    std::string content;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(real_fd, buf, sizeof(buf))) > 0) content.append(buf, n);
    ::close(real_fd);

    std::string filtered;
    filtered.reserve(content.size());
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("ternak_tt") != std::string::npos) continue;
        if (line.find("ternak-tt") != std::string::npos) continue;
        filtered.append(line);
        filtered.push_back('\n');
    }

    int mfd = tt_memfd_create("clean", MFD_CLOEXEC);
    if (mfd < 0) {
        tt_in_openat_hook = false;
        return orig_openat(dirfd, path, flags, mode);
    }
    if (!filtered.empty()) ::write(mfd, filtered.data(), filtered.size());
    ::lseek(mfd, 0, SEEK_SET);
    tt_in_openat_hook = false;
    return mfd;
}

static bool find_libc_dev_inode(dev_t* dev_out, ino_t* ino_out) {
    FILE* f = ::fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (::fgets(line, sizeof(line), f)) {
        char* nl = ::strchr(line, '\n');
        if (nl) *nl = 0;
        char* sp = ::strrchr(line, ' ');
        if (!sp) continue;
        char* path = sp + 1;
        size_t plen = ::strlen(path);
        if (plen < 8) continue;
        if (::strcmp(path + plen - 8, "/libc.so") != 0) continue;
        unsigned long a1, a2, off;
        char perms[8] = {0};
        unsigned int dmaj = 0, dmin = 0;
        unsigned long ino = 0;
        if (::sscanf(line, "%lx-%lx %7s %lx %x:%x %lu",
                     &a1, &a2, perms, &off, &dmaj, &dmin, &ino) == 7) {
            *dev_out = (dev_t)((dmaj << 8) | dmin);
            *ino_out = (ino_t)ino;
            found = true;
            break;
        }
    }
    ::fclose(f);
    return found;
}

using api_level_t = int (*)();
static api_level_t orig_api_level = nullptr;

static int hook_api_level() {
    const std::string& sdk_str = val("SDK_INT");
    if (!sdk_str.empty()) {
        int spoofed = std::atoi(sdk_str.c_str());
        if (spoofed > 0) return spoofed;
    }
    return orig_api_level ? orig_api_level() : 0;
}

static void install_proc_sanitizer(Api* api) {
    if (!api) return;
    dev_t dev = 0;
    ino_t ino = 0;
    if (!find_libc_dev_inode(&dev, &ino)) {
        LOGI("proc sanitizer: libc.so not found in maps (skip)");
        return;
    }
    api->pltHookRegister(dev, ino, "openat",
                         reinterpret_cast<void*>(hook_openat),
                         reinterpret_cast<void**>(&orig_openat));
    api->pltHookRegister(dev, ino, "__openat",
                         reinterpret_cast<void*>(hook_openat),
                         reinterpret_cast<void**>(&orig_openat));
    api->pltHookRegister(dev, ino, "android_get_device_api_level",
                         reinterpret_cast<void*>(hook_api_level),
                         reinterpret_cast<void**>(&orig_api_level));
    if (!api->pltHookCommit()) {
        LOGI("proc sanitizer: PLT commit false (best-effort skipped)");
    } else {
        LOGI("proc sanitizer installed (dev=%lu ino=%lu)",
             (unsigned long)dev, (unsigned long)ino);
    }
}

static struct sigaction g_prev_sig[NSIG];
static char g_watchdog_pkg[128] = "?";
static long g_load_time_ms = 0;
static int  g_crash_log_fd = -1;
static volatile sig_atomic_t g_crash_count[NSIG] = {0};
static const int CRASH_LIMIT = 3;

static long tt_now_ms() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}
static const char* tt_sig_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGABRT: return "SIGABRT";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGSYS:  return "SIGSYS";
        case SIGTERM: return "SIGTERM";
        case SIGPIPE: return "SIGPIPE";
        default:      return "?";
    }
}

static void tt_signal_handler(int sig, siginfo_t* info, void*  ) {
    int n = 0;
    if (sig >= 0 && sig < NSIG) {
        g_crash_count[sig] = (sig_atomic_t)(g_crash_count[sig] + 1);
        n = g_crash_count[sig];
    }

    if (n <= CRASH_LIMIT) {
        long alive = tt_now_ms() - g_load_time_ms;
        char buf[256];
        int len = ::snprintf(buf, sizeof(buf),
            "[ternak_tt/%s] CRASH pkg=%s pid=%d sig=%d(%s) code=%d addr=%p sender=%d alive=%ldms hit=%d/%d\n",
            TT_VARIANT_TAG,
            g_watchdog_pkg, getpid(),
            sig, tt_sig_name(sig),
            info ? info->si_code : 0,
            info ? info->si_addr : nullptr,
            info ? info->si_pid  : 0,
            alive, n, CRASH_LIMIT);
        if (len > 0) {
            if (g_crash_log_fd >= 0) ::write(g_crash_log_fd, buf, (size_t)len);

            ::write(2, buf, (size_t)len);
        }
    }

    if (sig >= 0 && sig < NSIG) {
        struct sigaction* p = &g_prev_sig[sig];
        bool prev_is_real =
            ((p->sa_flags & SA_SIGINFO) && p->sa_sigaction != nullptr) ||
            (!(p->sa_flags & SA_SIGINFO) &&
             p->sa_handler != SIG_DFL && p->sa_handler != SIG_IGN &&
             p->sa_handler != nullptr);
        if (prev_is_real) {
            sigaction(sig, p, nullptr);
        } else {
            struct sigaction dfl;
            memset(&dfl, 0, sizeof(dfl));
            dfl.sa_handler = SIG_DFL;
            sigaction(sig, &dfl, nullptr);
        }
    }

}

static void install_crash_watchdog(const std::string& pkg) {
    ::snprintf(g_watchdog_pkg, sizeof(g_watchdog_pkg), "%s", pkg.c_str());
    g_load_time_ms = tt_now_ms();

    if (g_crash_log_fd < 0) {
        g_crash_log_fd = ::open("/dev/kmsg",
                                O_WRONLY | O_CLOEXEC | O_NONBLOCK);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sa.sa_sigaction = tt_signal_handler;
    sigemptyset(&sa.sa_mask);

    static const int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL, SIGSYS };
    for (int s : sigs) {
        g_crash_count[s] = 0;
        sigaction(s, &sa, &g_prev_sig[s]);
    }
    LOGD("crash watchdog armed for %s (6 signals, limit=%d)",
         pkg.c_str(), CRASH_LIMIT);
}

class TernakTT : public zygisk::ModuleBase {
public:
    void onLoad(Api* api, JNIEnv* env) override {
        api_ = api; env_ = env;
        LOGD("onLoad build=%s version=%s api=%p pid=%d uid=%d",
             TT_VARIANT_TAG, TT_VERSION_STR, api, getpid(), getuid());
    }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        std::string pkg;
        if (args && args->nice_name) {
            tt::JniStringGuard pkg_guard(env_, args->nice_name);
            pkg = pkg_guard.str();
        }
        LOGD("preAppSpecialize pkg='%s' pid=%d", pkg.c_str(), getpid());
        if (pkg.empty()) { unload(); return; }

        int fd = api_->connectCompanion();
        if (fd < 0) { unload(); return; }

        uint8_t cmd = CMD_GET_IDENTITY;
        ::write(fd, &cmd, 1);
        uint16_t plen = (uint16_t)pkg.size();
        ::write(fd, &plen, sizeof(plen));
        if (plen) ::write(fd, pkg.data(), plen);

        uint32_t len = 0;
        if (::read(fd, &len, sizeof(len)) != (ssize_t)sizeof(len) || len > 65536) {
            ::close(fd); unload(); return;
        }
        if (len == 0) { ::close(fd); unload(); return; }

        blob_.resize(len);
        size_t got = 0;
        while (got < len) {
            ssize_t n = ::read(fd, blob_.data() + got, len - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        ::close(fd);
        if (got != len) { unload(); return; }

        active_ = true;
        pkg_ = pkg;
        LOGI("target: %s (%u B) [%s]", pkg.c_str(), len, TT_VARIANT_TAG);

        request_companion_mounts(api_);
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!active_) return;
        parse_blob();
        LOGD("parse_blob: %zu identity keys loaded", g_id.size());

        install_build_hook(env_);

        JNINativeMethod sp_methods[] = {
            {const_cast<char*>("native_get"),
             const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
             reinterpret_cast<void*>(hook_prop_get)},
            {const_cast<char*>("native_get_int"),
             const_cast<char*>("(Ljava/lang/String;I)I"),
             reinterpret_cast<void*>(hook_prop_get_int)},
            {const_cast<char*>("native_get_long"),
             const_cast<char*>("(Ljava/lang/String;J)J"),
             reinterpret_cast<void*>(hook_prop_get_long)},
            {const_cast<char*>("native_get_boolean"),
             const_cast<char*>("(Ljava/lang/String;Z)Z"),
             reinterpret_cast<void*>(hook_prop_get_bool)},
        };
        api_->hookJniNativeMethods(env_, "android/os/SystemProperties",
                                   sp_methods, 4);
        LOGD("L2+L7 SystemProperties hooks installed (%d methods)", 4);

        JNINativeMethod build_methods[] = {
            {const_cast<char*>("getRadioVersion"),
             const_cast<char*>("()Ljava/lang/String;"),
             reinterpret_cast<void*>(hook_build_radio)},
        };
        api_->hookJniNativeMethods(env_, "android/os/Build", build_methods, 1);

        install_proc_sanitizer(api_);

        install_crash_watchdog(pkg_);
    }

    void preServerSpecialize(ServerSpecializeArgs*) override { unload(); }

private:
    Api*                 api_    = nullptr;
    JNIEnv*              env_    = nullptr;
    std::string          pkg_;
    bool                 active_ = false;
    std::vector<uint8_t> blob_;

    void unload() {
        if (api_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

    static std::string rtrim(std::string s) {
        while (!s.empty()) {
            char c = s.back();
            if (c == '\r' || c == '\n' || c == '\t' || c == ' ') s.pop_back();
            else break;
        }
        return s;
    }

    void parse_blob() {
        std::string s(blob_.begin(), blob_.end());
        std::istringstream iss(s);
        std::string line;
        while (std::getline(iss, line)) {
            line = rtrim(std::move(line));
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = rtrim(line.substr(eq + 1));
            g_id[k] = v;
        }
    }
};

REGISTER_ZYGISK_MODULE(TernakTT)

extern "C" void ternak_tt_companion(int client);
REGISTER_ZYGISK_COMPANION(ternak_tt_companion)
