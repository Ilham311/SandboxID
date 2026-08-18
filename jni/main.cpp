
// main.cpp — Ternak TT Zygisk module (in-process layer)
//
// Responsibilities in the app process:
//   L1  Build / Build.VERSION static-field injection (SetStaticObjectField)
//   L2  SystemProperties.native_get hook (Java prop reads) via hookJniNativeMethods
//   L3  (opt-in, TT_ENABLE_LSPLANT) Settings.Secure.getString ART-hook via LSPlant
//       — deterministic per-persona ANDROID_ID. DEFAULT OFF (boot-loop risk until
//       build+boot verified); see tt_lsplant.hpp.
//   L7  (debug only) leak sensors on native_get_int/long/boolean
//   +   trigger the companion's per-app build.prop bind-mounts, in postAppSpecialize
//   +   async-signal-safe crash watchdog (diagnostic)
//
// What this file deliberately does NOT do (see report / git history):
//   - No /proc openat "sanitizer": PLT-hooking libc's own dev/inode cannot
//     intercept libc-internal open()/openat() (fopen→open is intra-libc), so it
//     was a double no-op. Mount concealment is delegated to the Zygisk provider
//     (ReZygisk/NeoZygisk/Zygisk-Assistant) + FORCE_DENYLIST_UNMOUNT.
//   - No WifiInfo / TelephonyManager hooks: getMacAddress() is already anonymized
//     to 02:00:00:00:00:00 for non-privileged apps (API 23+), and getImei/
//     getSubscriberId/getSimSerialNumber need READ_PRIVILEGED_PHONE_STATE (API
//     29+) so third-party apps get SecurityException before the body runs — a
//     spoofed value there is anomalous, not helpful. See tt_lsplant.hpp skip notes.

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
#include <android/log.h>
#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <signal.h>
#include <ctime>
#include <thread>
#include "zygisk.hpp"
#include "tt_config.hpp"
#include "tt_lsplant.hpp"   // L3 (opt-in, TT_ENABLE_LSPLANT): no-op stubs when OFF

#define LOG_TAG "TernakTT"
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

// Identity table for the current process. Populated once from the companion
// blob in postAppSpecialize (each app is a separate forked process, so a global
// is per-process and needs no locking).
static std::map<std::string, std::string> g_id;

// Resolve an identity key: prefer the parsed blob, then the auditable fallbacks
// in tt::VAL_DEFAULTS. Returns a reference into stable storage.
static const std::string& val(const std::string& k) {
    static const std::string empty;
    auto it = g_id.find(k);
    if (it != g_id.end() && !it->second.empty()) return it->second;

    // Build the fallback map once from the header table.
    static const std::map<std::string, std::string> defaults = [] {
        std::map<std::string, std::string> m;
        for (size_t i = 0; i < tt::VAL_DEFAULTS_N; ++i)
            m.emplace(tt::VAL_DEFAULTS[i].k, tt::VAL_DEFAULTS[i].v);
        return m;
    }();
    auto d = defaults.find(k);
    if (d != defaults.end()) return d->second;
    return empty;
}

// ============================================================ L2: prop hook ==
// Original SystemProperties.native_get, captured by hookJniNativeMethods so we
// can chain unmapped keys to the real reader instead of fabricating a value.
static jstring (*orig_native_get)(JNIEnv*, jclass, jstring, jstring) = nullptr;

static jstring hook_prop_get(JNIEnv* env, jclass clazz, jstring j_key, jstring j_def) {
    if (!j_key) return j_def;
    const char* raw = env->GetStringUTFChars(j_key, nullptr);
    std::string k(raw ? raw : "");
    env->ReleaseStringUTFChars(j_key, raw);
    LOGD("L2 native_get('%s')", k.c_str());

    // ro.* / gsm.* key -> identity key. ro.hardware and ro.board.platform are
    // now driven from the persona (Tensor codename), never a hard-coded SoC.
    static const std::map<std::string, std::string> map = {
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
    };

    auto it = map.find(k);
    if (it != map.end()) {
        const std::string& v = val(it->second);
        if (!v.empty()) {
            LOGD("L2 SPOOF '%s' -> '%s'", k.c_str(), v.c_str());
            return env->NewStringUTF(v.c_str());
        }
    }
    // Identity-independent, persona-consistent static answers.
    for (size_t i = 0; i < tt::STATIC_PROP_DEFAULTS_N; ++i) {
        if (k == tt::STATIC_PROP_DEFAULTS[i].k) {
            LOGD("L2 SPOOF-STATIC '%s' -> '%s'", k.c_str(), tt::STATIC_PROP_DEFAULTS[i].v);
            return env->NewStringUTF(tt::STATIC_PROP_DEFAULTS[i].v);
        }
    }
    // Unmapped: chain to the real native_get (reads the prop store, already
    // globally spoofed by resetprop for the ro.* identity keys).
    if (orig_native_get) return orig_native_get(env, clazz, j_key, j_def);
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(k.c_str(), buf) > 0) return env->NewStringUTF(buf);
    return j_def;
}

// Install L2 via the Zygisk-coordinated API (plays nice with other modules that
// also hook SystemProperties) and capture the original for chaining.
static void install_prop_hook(Api* api, JNIEnv* env) {
    JNINativeMethod m = {
        const_cast<char*>("native_get"),
        const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
        reinterpret_cast<void*>(hook_prop_get),
    };
    api->hookJniNativeMethods(env, "android/os/SystemProperties", &m, 1);
    // hookJniNativeMethods writes the previous fnPtr back into m.fnPtr.
    orig_native_get = reinterpret_cast<jstring (*)(JNIEnv*, jclass, jstring, jstring)>(m.fnPtr);
    if (orig_native_get)
        LOGD("L2 native_get hooked (orig=%p)", reinterpret_cast<void*>(orig_native_get));
    else
        LOGE("L2 native_get hook did not bind (method missing?)");
}

// ==================================================== L7: leak sensors (dbg) ==
#ifdef TT_DEBUG
static jint     (*orig_get_int)(JNIEnv*, jclass, jstring, jint)     = nullptr;
static jlong    (*orig_get_long)(JNIEnv*, jclass, jstring, jlong)   = nullptr;
static jboolean (*orig_get_bool)(JNIEnv*, jclass, jstring, jboolean)= nullptr;

static const std::map<std::string, jboolean>& tt_bool_spoof() {
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
static const std::map<std::string, jint>& tt_int_spoof() {
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
static const std::map<std::string, jlong>& tt_long_spoof() {
    static const std::map<std::string, jlong> m = {
        {"ro.gfx.driver_build_time",              1704067200LL},
    };
    return m;
}
static bool tt_should_suppress_key(const std::string& k) {
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
    std::string k(r ? r : "");
    env->ReleaseStringUTFChars(j_key, r);
    const auto& m = tt_int_spoof();
    auto it = m.find(k);
    if (it != m.end()) { LOGD("L7 SPI '%s' -> %d", k.c_str(), it->second); return it->second; }
    if (tt_should_suppress_key(k)) { LOGD("L7 SPI SUPPRESS '%s'", k.c_str()); return def; }
    return orig_get_int ? orig_get_int(env, clazz, j_key, def) : def;
}
static jlong hook_prop_get_long(JNIEnv* env, jclass clazz, jstring j_key, jlong def) {
    if (!j_key) return def;
    const char* r = env->GetStringUTFChars(j_key, nullptr);
    std::string k(r ? r : "");
    env->ReleaseStringUTFChars(j_key, r);
    const auto& m = tt_long_spoof();
    auto it = m.find(k);
    if (it != m.end()) { LOGD("L7 SPL '%s' -> %lld", k.c_str(), (long long)it->second); return it->second; }
    if (tt_should_suppress_key(k)) { LOGD("L7 SPL SUPPRESS '%s'", k.c_str()); return def; }
    return orig_get_long ? orig_get_long(env, clazz, j_key, def) : def;
}
static jboolean hook_prop_get_bool(JNIEnv* env, jclass clazz, jstring j_key, jboolean def) {
    if (!j_key) return def;
    const char* r = env->GetStringUTFChars(j_key, nullptr);
    std::string k(r ? r : "");
    env->ReleaseStringUTFChars(j_key, r);
    const auto& m = tt_bool_spoof();
    auto it = m.find(k);
    if (it != m.end()) { LOGD("L7 SPB '%s' -> %d", k.c_str(), (int)it->second); return it->second; }
    if (tt_should_suppress_key(k)) { LOGD("L7 SPB SUPPRESS '%s'", k.c_str()); return def; }
    return orig_get_bool ? orig_get_bool(env, clazz, j_key, def) : def;
}

// NOTE: Build.getRadioVersion() is intentionally NOT hooked — it is a plain Java
// wrapper over SystemProperties.get("gsm.version.baseband"), which the L2 hook
// already covers. RegisterNatives on it fails silently (it isn't native).
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
    orig_get_int  = reinterpret_cast<jint (*)(JNIEnv*, jclass, jstring, jint)>(m[0].fnPtr);
    orig_get_long = reinterpret_cast<jlong (*)(JNIEnv*, jclass, jstring, jlong)>(m[1].fnPtr);
    orig_get_bool = reinterpret_cast<jboolean (*)(JNIEnv*, jclass, jstring, jboolean)>(m[2].fnPtr);
    LOGD("L7 leak sensors installed (int/long/bool)");
}
#endif // TT_DEBUG

// ================================================== crash watchdog (AS-safe) ==
// The signal handler must be async-signal-safe: it may NOT call
// __android_log_print / malloc / std::string ops. So it only clock_gettime()s,
// packs a fixed POD, does ONE non-blocking write() to a self-pipe, then chains
// to the previous handler. A normal drain thread reads the pipe and LOGE()s.
// (signal-safety(7): write/clock_gettime/sigaction/raise/sched_yield are safe.)
struct TtCrashRec {
    uint32_t magic;
    int32_t  sig;
    int32_t  code;
    int32_t  sender;
    int32_t  pid;
    int32_t  hit;
    int64_t  alive_ms;
    void*    addr;
};
static const uint32_t TT_CRASH_MAGIC = 0x54544352u; // 'TTCR'

static int         g_crash_pipe[2] = {-1, -1};
static char        g_watchdog_pkg_buf[128] = {0};
static int64_t     g_load_time_ms = 0;
static struct sigaction g_prev_sig[NSIG];
static volatile sig_atomic_t g_crash_count[NSIG] = {0};
static const int   CRASH_LIMIT = 3;

static int64_t tt_now_ms() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static const char* tt_sig_name(int sig) {
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

// Normal-context thread: safe to use liblog here.
static void tt_crash_drain_loop() {
    TtCrashRec rec;
    while (tt::read_full(g_crash_pipe[0], &rec, sizeof(rec))) {
        if (rec.magic != TT_CRASH_MAGIC) continue;
        LOGE("CRASH [%s] pkg=%s pid=%d signal=%d(%s) code=%d addr=%p sender=%d alive=%lldms hit=%d/%d",
             TT_VARIANT_TAG, g_watchdog_pkg_buf, rec.pid, rec.sig, tt_sig_name(rec.sig),
             rec.code, rec.addr, rec.sender, (long long)rec.alive_ms, rec.hit, CRASH_LIMIT);
    }
}

static void tt_signal_handler(int sig, siginfo_t* info, void* ctx) {
    int n = 0;
    if (sig >= 0 && sig < NSIG) n = ++g_crash_count[sig];

    if (n <= CRASH_LIMIT && g_crash_pipe[1] >= 0) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        TtCrashRec rec;
        rec.magic    = TT_CRASH_MAGIC;
        rec.sig      = sig;
        rec.code     = info ? info->si_code : 0;
        rec.sender   = info ? info->si_pid  : 0;
        rec.pid      = (int)getpid();
        rec.hit      = n;
        rec.alive_ms = ((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000) - g_load_time_ms;
        rec.addr     = info ? info->si_addr : nullptr;
        ssize_t wr = ::write(g_crash_pipe[1], &rec, sizeof(rec)); // non-blocking, best-effort
        (void)wr;
        sched_yield(); // give the drain thread a chance to log before we chain
    }

    // Chain to the previous handler (sigchain shim -> debuggerd) so tombstones
    // are still produced. Calling it directly (not restore-to-SIG_DFL) preserves
    // the debuggerd chain, per art/sigchainlib.
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

// Diagnostic only. SIGSYS is intentionally NOT hooked: zygote's seccomp policy
// raises SIGSYS on violations, and shadowing it would mask real seccomp events.
static void install_crash_watchdog(const std::string& pkg) {
    static bool armed = false;
    if (armed) return;

    strncpy(g_watchdog_pkg_buf, pkg.c_str(), sizeof(g_watchdog_pkg_buf) - 1);
    g_load_time_ms = tt_now_ms();

    if (pipe2(g_crash_pipe, O_CLOEXEC) == 0) {
        int fl = fcntl(g_crash_pipe[1], F_GETFL, 0);
        if (fl >= 0) fcntl(g_crash_pipe[1], F_SETFL, fl | O_NONBLOCK); // never block in handler
        std::thread(tt_crash_drain_loop).detach();
    } else {
        g_crash_pipe[0] = g_crash_pipe[1] = -1; // no pipe: handler just chains, no logging
        LOGE("crash watchdog: pipe2 failed errno=%d (logging disabled, chaining still armed)", errno);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sa.sa_sigaction = tt_signal_handler;
    sigemptyset(&sa.sa_mask);

    // Armed set is deliberately {ABRT,FPE,ILL} and does NOT include SEGV/BUS —
    // this holds for the L3/LSPlant paths too. This watchdog is diagnostic (log
    // + chain to debuggerd), not a crash preventer; the L3 boot guard is
    // "default-OFF + fail-safe return false", never signal-catching. ART raises
    // SIGSEGV for its own implicit null / stack-overflow / suspend / GC checks
    // and recovers via its chained handler, so arming SEGV/BUS here would log
    // those benign faults as "CRASH" (misleading even under CRASH_LIMIT). The
    // signals an L3 failure actually surfaces ARE armed: SIGABRT (ART
    // CHECK/LOG(FATAL) / libc abort) and SIGILL (control jumping into a broken
    // Dobby trampoline). tt_sig_name() still names SEGV/BUS/SYS for the rare case
    // a chained-to handler re-enters us; that naming table is not the armed set.
    static const int sigs[] = { SIGABRT, SIGFPE, SIGILL };
    for (int s : sigs) {
        g_crash_count[s] = 0;
        sigaction(s, &sa, &g_prev_sig[s]);
    }
    armed = true;
    LOGD("crash watchdog armed for %s (ABRT/FPE/ILL, limit=%d)", pkg.c_str(), CRASH_LIMIT);
}

// ============================================ L1: Build static-field inject ==
// Every JNI call after Get*FieldID is guarded with ExceptionCheck+ExceptionClear.
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
        static const std::pair<const char*, const char*> f[] = {
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
    if (ver && !env->ExceptionCheck()) {
        set_str(env, ver, "RELEASE",        val("RELEASE"));
        set_str(env, ver, "INCREMENTAL",    val("INCREMENTAL"));
        set_str(env, ver, "SECURITY_PATCH", val("SECURITY_PATCH"));
        // SDK_INT is only injected with a persona whose SDK does NOT exceed the
        // real device's SDK — guaranteed by ternak-tt's SDK-safe persona
        // selection, which filters the pool to sdk <= device sdk (downgrade is
        // safe, upgrade is not). Injecting a *higher* SDK than the OS actually
        // is would make apps call framework APIs that don't exist -> crash.
        const std::string& s = val("SDK_INT");
        if (!s.empty()) {
            int sdk = std::atoi(s.c_str());
            if (sdk > 0) set_int(env, ver, "SDK_INT", sdk);
        }
        env->DeleteLocalRef(ver);
    } else env->ExceptionClear();
}

// ================================================ companion bind-mount call ==
// Reuses the socket opened (and exemptFd'd) in preAppSpecialize. Called from
// postAppSpecialize so the app has already unshared its mount namespace.
static void request_companion_mounts(int fd) {
    uint8_t cmd  = tt::CMD_DO_MOUNTS;
    uint32_t pid = (uint32_t)::getpid();
    if (!tt::write_full(fd, &cmd, 1) || !tt::write_full(fd, &pid, sizeof(pid))) {
        LOGE("companion DO_MOUNTS write failed (socket unusable post-specialize?)");
        return;
    }
    uint32_t ok = 0;
    if (!tt::read_full(fd, &ok, sizeof(ok))) { LOGE("companion mount ack failed"); return; }
    LOGI("bind-mount via companion: %u ok (pid=%u)", ok, pid);
}

// ================================================================== module ==
class TernakTT : public zygisk::ModuleBase {
public:
    void onLoad(Api* api, JNIEnv* env) override {
        api_ = api; env_ = env;
        LOGD("onLoad build=%s pid=%d uid=%d", TT_VARIANT_TAG, getpid(), getuid());
    }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        std::string pkg;
        if (args && args->nice_name) {
            const char* raw = env_->GetStringUTFChars(args->nice_name, nullptr);
            pkg = raw ? raw : "";
            env_->ReleaseStringUTFChars(args->nice_name, raw);
        }
        LOGD("preAppSpecialize pkg='%s' pid=%d", pkg.c_str(), getpid());
        if (pkg.empty()) { unload(); return; }

        int fd = api_->connectCompanion();
        LOGD("connectCompanion() -> fd=%d", fd);
        if (fd < 0) { unload(); return; }
        // Keep this socket usable across the specialize boundary (fd sanitizer /
        // denylist unmount would otherwise close it before postAppSpecialize).
        api_->exemptFd(fd);

        uint8_t cmd   = tt::CMD_GET_IDENTITY;
        uint16_t plen = (uint16_t)pkg.size();
        if (!tt::write_full(fd, &cmd, 1) ||
            !tt::write_full(fd, &plen, sizeof(plen)) ||
            (plen && !tt::write_full(fd, pkg.data(), plen))) {
            ::close(fd); unload(); return;
        }

        uint32_t len = 0;
        if (!tt::read_full(fd, &len, sizeof(len)) || len > tt::MAX_IDENTITY_BLOB) {
            ::close(fd); unload(); return;
        }
        if (len == 0) { // not a target (or identity genuinely absent after retries)
            LOGD("pkg='%s' not a target", pkg.c_str());
            ::close(fd); unload(); return;
        }

        blob_.resize(len);
        if (!tt::read_full(fd, blob_.data(), len)) { ::close(fd); unload(); return; }

        active_  = true;
        pkg_     = pkg;
        comp_fd_ = fd; // DO NOT close: reused for CMD_DO_MOUNTS in postAppSpecialize
        LOGI("target: %s (%u B) [%s]", pkg.c_str(), len, TT_VARIANT_TAG);
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!active_) return;

        parse_blob();
        LOGD("parse_blob: %zu identity keys", g_id.size());

        install_build_hook(env_);          // L1
        install_prop_hook(api_, env_);     // L2
#ifdef TT_DEBUG
        install_leak_sensors(api_, env_);  // L7
        for (auto& kv : g_id) LOGD("  [id] %s = %s", kv.first.c_str(), kv.second.c_str());
#endif
        install_crash_watchdog(pkg_);

#ifdef TT_ENABLE_LSPLANT
        // L3 (opt-in, default OFF): ART-hook Settings.Secure.getString so the app
        // sees a deterministic per-persona ANDROID_ID (the file-mount path is
        // unreliable for android_id — on API 26+ it lives in the per-app ssaid
        // table, not settings_secure.xml). Installed LAST and FULLY fail-safe:
        // if LSPlant Init/Hook fails on this device, L1/L2/L7 still stand and the
        // process continues normally (never abort, never half-unload).
        if (ttlsp::init(env_)) {
            if (!ttlsp::hook_android_id(env_, val("ANDROID_ID")))
                LOGE("L3 ANDROID_ID hook not installed (continuing with L1/L2)");
        } else {
            LOGE("L3 LSPlant init failed (continuing with L1/L2)");
        }
#endif

        // Trigger the bind-mounts HERE (not in preAppSpecialize). By post-specialize
        // the app has its OWN mount namespace (unshared in SpecializeCommon), so the
        // companion joins the app's ns and the overlay cannot leak into zygote —
        // which would otherwise spoof every subsequently-spawned app. See report.
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
        // Safe here: called only before any hook is installed (non-targets).
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
            g_id[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
};

REGISTER_ZYGISK_MODULE(TernakTT)

extern "C" void ternak_tt_companion(int client);
REGISTER_ZYGISK_COMPANION(ternak_tt_companion)
