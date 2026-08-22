





#include <jni.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <sys/system_properties.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <android/log.h>
#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <signal.h>
#include <ctime>
#include <thread>
#include "zygisk.hpp"
#include "config.hpp"
#include "sbx_lsplant.hpp"

#define LOG_TAG "SandboxID"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// WHY: LOGD now maps to the real ANDROID_LOG_DEBUG priority (was ANDROID_LOG_INFO),
// so debug traces are filterable as DEBUG and don't masquerade as INFO in logcat.
#ifdef SBX_DEBUG
#define LOGD(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "[D] " fmt, ##__VA_ARGS__)
#define SBX_VARIANT_TAG "debug"
#else
#define LOGD(...) ((void)0)
#define SBX_VARIANT_TAG "release"
#endif

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;




static constexpr struct timeval SBX_IO_TIMEOUT = {2, 0};





static std::map<std::string, std::string> g_id;


static const std::string& val(const std::string& k) {
    static const std::string empty;
    auto it = g_id.find(k);
    if (it != g_id.end() && !it->second.empty()) return it->second;

    static const std::map<std::string, std::string> defaults = [] {
        std::map<std::string, std::string> m;
        for (size_t i = 0; i < sandboxid::VAL_DEFAULTS_N; ++i)
            m.emplace(sandboxid::VAL_DEFAULTS[i].k, sandboxid::VAL_DEFAULTS[i].v);
        return m;
    }();
    auto d = defaults.find(k);
    if (d != defaults.end()) return d->second;
    return empty;
}





static jstring (*orig_native_get)(JNIEnv*, jclass, jstring, jstring) = nullptr;


static const std::map<std::string, std::string>& prop_to_identity_map() {
    static const std::map<std::string, std::string> m = {
        {"ro.serialno",                     "SERIAL"},
        {"ro.boot.serialno",                "SERIAL"},
        {"ro.build.fingerprint",            "FINGERPRINT"},
        {"ro.bootimage.build.fingerprint",  "FINGERPRINT"},
        {"ro.product.model",                "MODEL"},
        {"ro.product.brand",                "BRAND"},
        {"ro.product.manufacturer",         "MANUFACTURER"},
        {"ro.product.device",               "DEVICE"},
        {"ro.product.name",                 "PRODUCT"},
        {"ro.product.marketname",           "MARKETNAME"},
        {"ro.product.vendor.marketname",    "MARKETNAME"},
        {"ro.product.board",                "BOARD"},
        {"ro.hardware",                     "HARDWARE"},
        {"ro.board.platform",               "BOARD_PLATFORM"},
        {"ro.build.id",                     "ID"},
        {"ro.build.display.id",             "DISPLAY"},
        {"ro.build.description",            "DESCRIPTION"},
        {"ro.build.version.release",        "RELEASE"},
        {"ro.build.version.sdk",            "SDK_INT"},
        {"ro.build.version.security_patch", "SECURITY_PATCH"},
        {"ro.build.version.incremental",    "INCREMENTAL"},
        {"gsm.version.baseband",            "RADIO"},
        {"sys.boot_completed",              "SYS_BOOT_COMPLETED"},
        {"debug.force_rtl",                 "DEBUG_FORCE_RTL"},
        {"persist.radio.multisim.config",   "MULTISIM_CONFIG"},
        {"gsm.operator.numeric",            "GSM_OPERATOR_NUMERIC"},
        {"gsm.sim.operator.numeric",        "GSM_OPERATOR_NUMERIC"},
        {"gsm.operator.alpha",              "GSM_OPERATOR_ALPHA"},
        {"gsm.sim.operator.alpha",          "GSM_OPERATOR_ALPHA"},
        {"gsm.operator.iso-country",        "GSM_OPERATOR_ISO"},
        {"gsm.sim.operator.iso-country",    "GSM_OPERATOR_ISO"},
        {"ro.build.characteristics",        "BUILD_CHARACTERISTICS"},
        {"persist.sys.timezone",            "PERSIST_TIMEZONE"},
        {"ro.product.cpu.abi",              "CPU_ABI"},
        {"ro.product.cpu.abi2",             "CPU_ABI2"},
        {"ro.product.cpu.abilist",          "CPU_ABILIST"},
        {"ro.product.cpu.abilist64",        "CPU_ABILIST64"},
        {"ro.product.cpu.abilist32",        "CPU_ABILIST32"},
        {"dalvik.vm.heapgrowthlimit",       "DALVIK_HEAPGROWTHLIMIT"},
        {"ro.mediacodec.min_sample_rate",   "MEDIACODEC_MIN_RATE"},
        {"ro.mediacodec.max_sample_rate",   "MEDIACODEC_MAX_RATE"},
        {"ro.build.user",                   "USER"},
        {"ro.build.host",                   "HOST"},
        {"ro.build.tags",                   "TAGS"},
        {"ro.build.type",                   "TYPE"},

        // WHY (L2 parity / defense-in-depth): mirror what the resetprop layer (L1) and
        // the build.prop bind-mount write, so an app reading these in-process via
        // SystemProperties.get() sees the same persona. Modern Build.* read the BARE
        // ro.product.* (mapped above); the per-partition variants + extra fingerprints
        // below are what fingerprint/attestation-style checks read directly.
        {"ro.build.product",                     "DEVICE"},
        {"ro.build.version.release_or_codename", "RELEASE"},
        {"ro.vendor.build.security_patch",       "SECURITY_PATCH"},

        {"ro.system.build.fingerprint",     "FINGERPRINT"},
        {"ro.vendor.build.fingerprint",     "FINGERPRINT"},
        {"ro.odm.build.fingerprint",        "FINGERPRINT"},
        {"ro.product.build.fingerprint",    "FINGERPRINT"},
        {"ro.system_ext.build.fingerprint", "FINGERPRINT"},

        {"ro.product.system.model",         "MODEL"},
        {"ro.product.vendor.model",         "MODEL"},
        {"ro.product.odm.model",            "MODEL"},
        {"ro.product.product.model",        "MODEL"},
        {"ro.product.system_ext.model",     "MODEL"},

        {"ro.product.system.brand",         "BRAND"},
        {"ro.product.vendor.brand",         "BRAND"},
        {"ro.product.odm.brand",            "BRAND"},
        {"ro.product.product.brand",        "BRAND"},
        {"ro.product.system_ext.brand",     "BRAND"},

        {"ro.product.system.manufacturer",     "MANUFACTURER"},
        {"ro.product.vendor.manufacturer",     "MANUFACTURER"},
        {"ro.product.odm.manufacturer",        "MANUFACTURER"},
        {"ro.product.product.manufacturer",    "MANUFACTURER"},
        {"ro.product.system_ext.manufacturer", "MANUFACTURER"},

        {"ro.product.system.device",        "DEVICE"},
        {"ro.product.vendor.device",        "DEVICE"},
        {"ro.product.odm.device",           "DEVICE"},
        {"ro.product.product.device",       "DEVICE"},
        {"ro.product.system_ext.device",    "DEVICE"},

        {"ro.product.system.name",          "PRODUCT"},
        {"ro.product.vendor.name",          "PRODUCT"},
        {"ro.product.odm.name",             "PRODUCT"},
        {"ro.product.product.name",         "PRODUCT"},
        {"ro.product.system_ext.name",      "PRODUCT"},
    };
    return m;
}

static jstring hook_prop_get(JNIEnv* env, jclass clazz, jstring j_key, jstring j_def) {
    if (!j_key) return j_def;
    // WHY: GetStringUTFChars returns non-null on success and can only fail (return null)
    // by throwing OOM. Check/clear ONLY on the null path — the old unconditional
    // ExceptionCheck ran on every hot-path call for no benefit. On failure, clear the
    // pending exception (so subsequent JNI calls stay valid) and fall back to default.
    const char* raw = env->GetStringUTFChars(j_key, nullptr);
    if (!raw) { if (env->ExceptionCheck()) env->ExceptionClear(); return j_def; }
    std::string k(raw);
    env->ReleaseStringUTFChars(j_key, raw);
    LOGD("L2 native_get('%s')", k.c_str());

    
    const auto& map = prop_to_identity_map();
    auto it = map.find(k);
    if (it != map.end()) {
        const std::string& v = val(it->second);
        if (!v.empty()) {
            LOGD("L2 SPOOF '%s' -> '%s'", k.c_str(), v.c_str());
            return env->NewStringUTF(v.c_str());
        }
    }
    
    for (size_t i = 0; i < sandboxid::STATIC_PROP_DEFAULTS_N; ++i) {
        if (k == sandboxid::STATIC_PROP_DEFAULTS[i].k) {
            LOGD("L2 SPOOF-STATIC '%s' -> '%s'", k.c_str(), sandboxid::STATIC_PROP_DEFAULTS[i].v);
            return env->NewStringUTF(sandboxid::STATIC_PROP_DEFAULTS[i].v);
        }
    }
    
    if (orig_native_get) return orig_native_get(env, clazz, j_key, j_def);
    
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(k.c_str(), buf) > 0) return env->NewStringUTF(buf);
    return j_def;
}


static void install_prop_hook(Api* api, JNIEnv* env) {
    JNINativeMethod m = {
        const_cast<char*>("native_get"),
        const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
        reinterpret_cast<void*>(hook_prop_get),
    };
    api->hookJniNativeMethods(env, "android/os/SystemProperties", &m, 1);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("L2: JNI exception saat memasang native_get hook");
    }
    orig_native_get = reinterpret_cast<jstring (*)(JNIEnv*, jclass, jstring, jstring)>(m.fnPtr);
    if (orig_native_get)
        LOGD("L2 native_get hooked (orig=%p)", reinterpret_cast<void*>(orig_native_get));
    else
        LOGE("L2 native_get hook did not bind (method missing?)");
}




#ifdef SBX_DEBUG
static jint     (*orig_get_int)(JNIEnv*, jclass, jstring, jint)     = nullptr;
static jlong    (*orig_get_long)(JNIEnv*, jclass, jstring, jlong)   = nullptr;
static jboolean (*orig_get_bool)(JNIEnv*, jclass, jstring, jboolean)= nullptr;

static const std::map<std::string, jboolean>& sbx_bool_spoof() {
    static const std::map<std::string, jboolean> m = {
        {"sys.boot_completed",                       JNI_TRUE},
        {"debug.force_rtl",                          JNI_FALSE},
        {"framework.pause_bg_animations.enabled",    JNI_FALSE},
        {"dalvik.vm.dexopt.secondary",               JNI_TRUE},
        {"viewroot.profile_rendering",               JNI_FALSE},
        {"debug.sqlite.no_double_quoted_strs",       JNI_TRUE},
        {"persist.sys.activity_anim_perf_override",  JNI_FALSE},
        {"persist.sys.lmk.reportkills",              JNI_FALSE},
        {"debug.layout",                             JNI_FALSE},
    };
    return m;
}
static const std::map<std::string, jint>& sbx_int_spoof() {
    static const std::map<std::string, jint> m = {
        {"ro.mediacodec.min_sample_rate",         8000},
        {"ro.mediacodec.max_sample_rate",         192000},
        {"debug.sqlite.wal.autocheckpoint",       100},
        {"debug.sqlite.pagesize",                 4096},
        {"debug.sqlite.journalsizelimit",         524288},
        {"debug.sqlite.wal.truncatesize",         1048576},
        {"debug.sqlite.wal.poolsize",             0},
        {"debug.hwui.fps_divisor",                1},
        {"persist.wm.debug.ext_version_override", 0},
        {"build.version.extensions.r",            3},
        {"build.version.extensions.s",            4},
        {"build.version.extensions.t",            4},
        {"build.version.extensions.u",            13},
        {"build.version.extensions.v",            13},
        {"build.version.extensions.ad_services",  15},
        {"debug.am.run_gc_trim_level",            2147483647},
        {"debug.am.run_mallopt_trim_level",       2147483647},
        {"debug.adservices.binder_timeout",       10000},
    };
    return m;
}
static const std::map<std::string, jlong>& sbx_long_spoof() {
    static const std::map<std::string, jlong> m = {
        {"ro.gfx.driver_build_time",              1704067200LL},
    };
    return m;
}


static bool sbx_should_suppress_key(const std::string& k) {
    if (k.size() >= 11 + 5 &&
        k.compare(0, 11, "log.looper.") == 0 &&
        k.compare(k.size() - 5, 5, ".slow") == 0)
        return true;
    if (k.compare(0, 13, "debug.watson.") == 0)
        return true;
    return false;
}

static jint hook_prop_get_int(JNIEnv* env, jclass clazz, jstring j_key, jint def) {
    if (!j_key) return def;
    const char* r = env->GetStringUTFChars(j_key, nullptr);
    if (!r) { if (env->ExceptionCheck()) env->ExceptionClear(); return def; }
    std::string k(r);
    env->ReleaseStringUTFChars(j_key, r);
    const auto& m = sbx_int_spoof();
    auto it = m.find(k);
    if (it != m.end()) { LOGD("L7 SPI '%s' -> %d", k.c_str(), it->second); return it->second; }
    if (sbx_should_suppress_key(k)) { LOGD("L7 SPI SUPPRESS '%s'", k.c_str()); return def; }
    return orig_get_int ? orig_get_int(env, clazz, j_key, def) : def;
}
static jlong hook_prop_get_long(JNIEnv* env, jclass clazz, jstring j_key, jlong def) {
    if (!j_key) return def;
    const char* r = env->GetStringUTFChars(j_key, nullptr);
    if (!r) { if (env->ExceptionCheck()) env->ExceptionClear(); return def; }
    std::string k(r);
    env->ReleaseStringUTFChars(j_key, r);
    const auto& m = sbx_long_spoof();
    auto it = m.find(k);
    if (it != m.end()) { LOGD("L7 SPL '%s' -> %lld", k.c_str(), (long long)it->second); return it->second; }
    if (sbx_should_suppress_key(k)) { LOGD("L7 SPL SUPPRESS '%s'", k.c_str()); return def; }
    return orig_get_long ? orig_get_long(env, clazz, j_key, def) : def;
}
static jboolean hook_prop_get_bool(JNIEnv* env, jclass clazz, jstring j_key, jboolean def) {
    if (!j_key) return def;
    const char* r = env->GetStringUTFChars(j_key, nullptr);
    if (!r) { if (env->ExceptionCheck()) env->ExceptionClear(); return def; }
    std::string k(r);
    env->ReleaseStringUTFChars(j_key, r);
    const auto& m = sbx_bool_spoof();
    auto it = m.find(k);
    if (it != m.end()) { LOGD("L7 SPB '%s' -> %d", k.c_str(), (int)it->second); return it->second; }
    if (sbx_should_suppress_key(k)) { LOGD("L7 SPB SUPPRESS '%s'", k.c_str()); return def; }
    return orig_get_bool ? orig_get_bool(env, clazz, j_key, def) : def;
}


static void install_leak_sensors(Api* api, JNIEnv* env) {
    JNINativeMethod m[3] = {
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
    api->hookJniNativeMethods(env, "android/os/SystemProperties", m, 3);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("L7: JNI exception saat memasang leak sensors");
    }
    orig_get_int  = reinterpret_cast<jint (*)(JNIEnv*, jclass, jstring, jint)>(m[0].fnPtr);
    orig_get_long = reinterpret_cast<jlong (*)(JNIEnv*, jclass, jstring, jlong)>(m[1].fnPtr);
    orig_get_bool = reinterpret_cast<jboolean (*)(JNIEnv*, jclass, jstring, jboolean)>(m[2].fnPtr);
    LOGD("L7 leak sensors installed (int/long/bool)");
}
#endif 




struct SbxCrashRec {
    uint32_t magic;
    int32_t  sig;
    int32_t  code;
    int32_t  sender;
    int32_t  pid;
    int32_t  hit;
    int64_t  alive_ms;
    void*    addr;
};
static const uint32_t SBX_CRASH_MAGIC = 0x54544352u;

static int         g_crash_pipe[2] = {-1, -1};
static char        g_watchdog_pkg_buf[128] = {0};
static int64_t     g_load_time_ms = 0;
static struct sigaction g_prev_sig[NSIG];
static volatile sig_atomic_t g_crash_count[NSIG] = {0};
static const int   CRASH_LIMIT = 3;


static int64_t sbx_now_ms() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char* sbx_sig_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGSYS:  return "SIGSYS";
        default:      return "?";
    }
}


static void sbx_crash_drain_loop() {
    SbxCrashRec rec;
    while (sandboxid::read_full(g_crash_pipe[0], &rec, sizeof(rec))) {
        if (rec.magic != SBX_CRASH_MAGIC) continue;
        LOGE("CRASH [%s] pkg=%s pid=%d signal=%d(%s) code=%d addr=%p sender=%d alive=%lldms hit=%d/%d",
             SBX_VARIANT_TAG, g_watchdog_pkg_buf, rec.pid, rec.sig, sbx_sig_name(rec.sig),
             rec.code, rec.addr, rec.sender, (long long)rec.alive_ms, rec.hit, CRASH_LIMIT);
    }
}



static void sbx_signal_handler(int sig, siginfo_t* info, void* ctx) {
    int n = 0;
    if (sig >= 0 && sig < NSIG) {
        
        
        
        n = g_crash_count[sig] + 1;
        g_crash_count[sig] = n;
    }

    if (n <= CRASH_LIMIT && g_crash_pipe[1] >= 0) {
        // WHY: save/restore errno — a signal can interrupt code mid-syscall, and our
        // ::write here would otherwise clobber the interrupted frame's errno on return.
        // clock_gettime and write are async-signal-safe (signal-safety(7)); the old
        // sched_yield() was NOT on that safe list AND did nothing useful here (the drain
        // thread reads the pipe asynchronously), so it is removed.
        int saved_errno = errno;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        SbxCrashRec rec;
        rec.magic    = SBX_CRASH_MAGIC;
        rec.sig      = sig;
        rec.code     = info ? info->si_code : 0;
        rec.sender   = info ? info->si_pid  : 0;
        rec.pid      = (int)getpid();
        rec.hit      = n;
        rec.alive_ms = ((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000) - g_load_time_ms;
        rec.addr     = info ? info->si_addr : nullptr;
        ssize_t wr = ::write(g_crash_pipe[1], &rec, sizeof(rec));
        (void)wr;
        errno = saved_errno;
    }

    
    if (sig >= 0 && sig < NSIG) {
        struct sigaction* p = &g_prev_sig[sig];
        if ((p->sa_flags & SA_SIGINFO) && p->sa_sigaction) {
            p->sa_sigaction(sig, info, ctx);
        } else if (!(p->sa_flags & SA_SIGINFO) && p->sa_handler &&
                   p->sa_handler != SIG_DFL && p->sa_handler != SIG_IGN) {
            p->sa_handler(sig);
        } else {
            
            signal(sig, SIG_DFL);
            raise(sig);
        }
    }
}



static void install_crash_watchdog(const std::string& pkg) {
    static bool armed = false;
    if (armed) return;

    strncpy(g_watchdog_pkg_buf, pkg.c_str(), sizeof(g_watchdog_pkg_buf) - 1);
    g_load_time_ms = sbx_now_ms();

    if (pipe2(g_crash_pipe, O_CLOEXEC) == 0) {
        int fl = fcntl(g_crash_pipe[1], F_GETFL, 0);
        if (fl >= 0) fcntl(g_crash_pipe[1], F_SETFL, fl | O_NONBLOCK);
        std::thread(sbx_crash_drain_loop).detach();
    } else {
        g_crash_pipe[0] = g_crash_pipe[1] = -1;
        LOGE("crash watchdog: pipe2 failed errno=%d (logging disabled, chaining still armed)", errno);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    // WHY: no sigaltstack() is installed here (we don't own the app's alternate stack),
    // so SA_ONSTACK was a misleading no-op — dropped. We chain ABRT/FPE/ILL (not the
    // stack-overflow-prone SEGV/BUS), so running the tiny handler on the normal stack is
    // fine; installing a bespoke alt stack would add setup cost for no real benefit.
    sa.sa_flags     = SA_SIGINFO;
    sa.sa_sigaction = sbx_signal_handler;
    sigemptyset(&sa.sa_mask);

    static const int sigs[] = { SIGABRT, SIGFPE, SIGILL };
    for (int s : sigs) {
        g_crash_count[s] = 0;
        sigaction(s, &sa, &g_prev_sig[s]);
    }
    armed = true;
    LOGD("crash watchdog armed for %s (ABRT/FPE/ILL, limit=%d)", pkg.c_str(), CRASH_LIMIT);
}





static void set_str(JNIEnv* env, jclass c, const char* f, const std::string& v) {
    if (v.empty()) return;
    jfieldID id = env->GetStaticFieldID(c, f, "Ljava/lang/String;");
    if (!id || env->ExceptionCheck()) { env->ExceptionClear(); return; }
    jstring j = env->NewStringUTF(v.c_str());
    if (!j || env->ExceptionCheck()) { env->ExceptionClear(); return; }
    env->SetStaticObjectField(c, id, j);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(j);
}

static void set_int(JNIEnv* env, jclass c, const char* f, int v) {
    jfieldID id = env->GetStaticFieldID(c, f, "I");
    if (!id || env->ExceptionCheck()) { env->ExceptionClear(); return; }
    env->SetStaticIntField(c, id, v);
    if (env->ExceptionCheck()) env->ExceptionClear();
}


static void install_build_hook(JNIEnv* env) {
    jclass build = env->FindClass("android/os/Build");
    if (build && !env->ExceptionCheck()) {
        // WHY: Build.SERIAL is intentionally OMITTED here. On modern AOSP (API 26+) an
        // app without READ_PHONE_STATE reads Build.SERIAL as the literal "unknown" — it
        // does NOT reflect ro.serialno. Spoofing it to random hex is therefore LESS
        // realistic than leaving it "unknown"; ro.serialno/ro.boot.serialno are still set
        // natively (L1) for the (privileged) paths that actually read them.
        static const std::pair<const char*, const char*> f[] = {
            {"BRAND","BRAND"}, {"MANUFACTURER","MANUFACTURER"},
            {"MODEL","MODEL"}, {"DEVICE","DEVICE"}, {"PRODUCT","PRODUCT"},
            {"BOARD","BOARD"}, {"HARDWARE","HARDWARE"},
            {"FINGERPRINT","FINGERPRINT"}, {"ID","ID"},
            {"DISPLAY","DISPLAY"}, {"BOOTLOADER","BOOTLOADER"},
            {"HOST","HOST"}, {"USER","USER"}, {"TYPE","TYPE"},
            {"TAGS","TAGS"}, {"RADIO","RADIO"},
        };
        for (const auto& [fn, k] : f) set_str(env, build, fn, val(k));
        env->DeleteLocalRef(build);
    } else env->ExceptionClear();

    jclass ver = env->FindClass("android/os/Build$VERSION");
    if (ver && !env->ExceptionCheck()) {
        set_str(env, ver, "RELEASE",        val("RELEASE"));
        
        set_str(env, ver, "CODENAME",       std::string("REL"));
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





static void request_companion_mounts(int fd) {
    uint8_t cmd  = sandboxid::CMD_DO_MOUNTS;
    uint32_t pid = (uint32_t)::getpid();
    if (!sandboxid::write_full(fd, &cmd, 1) || !sandboxid::write_full(fd, &pid, sizeof(pid))) {
        // WHY WARN not ERROR: recoverable — the app still runs with L1/L2 spoofing, only
        // the build.prop bind-mount overlay is lost (often the exemptFd-false case above).
        LOGW("companion DO_MOUNTS write failed (socket unusable post-specialize?)");
        return;
    }
    uint32_t ok = 0;
    if (!sandboxid::read_full(fd, &ok, sizeof(ok))) { LOGW("companion mount ack failed"); return; }
    LOGI("bind-mount via companion: %u ok (pid=%u)", ok, pid);
}




class SandboxID : public zygisk::ModuleBase {
public:
    void onLoad(Api* api, JNIEnv* env) override {
        api_ = api; env_ = env;
        LOGD("onLoad build=%s pid=%d uid=%d", SBX_VARIANT_TAG, getpid(), getuid());
    }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        std::string pkg;
        if (args && args->nice_name) {
            const char* raw = env_->GetStringUTFChars(args->nice_name, nullptr);
            if (env_->ExceptionCheck()) env_->ExceptionClear();
            pkg = raw ? raw : "";
            if (raw) env_->ReleaseStringUTFChars(args->nice_name, raw);
        }
        LOGD("preAppSpecialize pkg='%s' pid=%d", pkg.c_str(), getpid());
        if (pkg.empty()) { unload(); return; }

        int fd = api_->connectCompanion();
        LOGD("connectCompanion() -> fd=%d", fd);
        if (fd < 0) { unload(); return; }

        
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &SBX_IO_TIMEOUT, sizeof(SBX_IO_TIMEOUT));
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &SBX_IO_TIMEOUT, sizeof(SBX_IO_TIMEOUT));

        // WHY: exemptFd tells zygote NOT to close this socket during the specialize fd
        // sweep — it is exactly what lets us reuse comp_fd_ in postAppSpecialize. A false
        // return means zygote WILL close it, so the later DO_MOUNTS would silently fail;
        // surface that here as a distinct, actionable warning rather than a generic EBADF.
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
        // WHY: do NOT emit the target package name at INFO in release — logcat is readable
        // by other on-device tooling and the name reveals which apps the user configured
        // (privacy). Keep a neutral INFO breadcrumb; the package name stays DEBUG-only.
        LOGI("target active (%u B) [%s]", len, SBX_VARIANT_TAG);
        LOGD("target pkg='%s'", pkg.c_str());
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!active_) return;

        parse_blob();
        LOGD("parse_blob: %zu identity keys", g_id.size());

        install_build_hook(env_);
        install_prop_hook(api_, env_);
#ifdef SBX_DEBUG
        install_leak_sensors(api_, env_);
        for (auto& kv : g_id) LOGD("  [id] %s = %s", kv.first.c_str(), kv.second.c_str());
#endif
        install_crash_watchdog(pkg_);

#ifdef SBX_ENABLE_LSPLANT
        if (sbxlsp::init(env_)) {
            if (!sbxlsp::hook_android_id(env_, val("ANDROID_ID")))
                LOGE("L3 ANDROID_ID hook not installed (continuing with L1/L2)");
        } else {
            LOGE("L3 LSPlant init failed (continuing with L1/L2)");
        }
#endif

        // WHY (do NOT move to preAppSpecialize): the app's PRIVATE mount namespace only
        // exists AFTER zygote's unshare(CLONE_NEWNS) during specialization. Requesting the
        // bind-mounts here (post) makes the companion setns() into THIS app's namespace;
        // issued from pre it would land in the still-shared zygote ns and either leak to
        // every process or be undone. The socket was opened in pre (connectCompanion is
        // pre-only under SELinux) and kept alive across specialize via exemptFd.
        if (comp_fd_ >= 0) {
            request_companion_mounts(comp_fd_);
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
            while (!v.empty() && (v.back()=='\r' || v.back()=='\n' || v.back()==' '))
                v.pop_back();
            if (!k.empty()) g_id[k] = v;
        }
    }
};

REGISTER_ZYGISK_MODULE(SandboxID)

extern "C" void sandboxid_companion(int client);
REGISTER_ZYGISK_COMPANION(sandboxid_companion)
