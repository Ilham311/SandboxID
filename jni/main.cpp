
#include <jni.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <android/log.h>
#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <signal.h>
#include <ctime>
#include "zygisk.hpp"
#include "java_hooks.hpp"

// v1.1.4: forward decl for early-skip helper defined near end of file.
static bool should_skip_early_v113(const std::string& pkg);

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#include <sys/syscall.h>
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

enum : uint8_t {
    CMD_CHECK_TT     = 1,
    CMD_GET_IDENTITY = 2,
    CMD_DO_MOUNTS    = 3,
};

static std::map<std::string, std::string> g_id;

static const std::string& val(const std::string& k) {
    static const std::string empty;
    auto it = g_id.find(k);
    if (it != g_id.end() && !it->second.empty()) return it->second;

    static const std::map<std::string, std::string> defaults = {
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
        {"MULTISIM_CONFIG",       ""},
    };
    auto d = defaults.find(k);
    if (d != defaults.end()) return d->second;
    return empty;
}

static jstring hook_prop_get(JNIEnv* env, jclass, jstring j_key, jstring j_def) {
    if (!j_key) return j_def;
    const char* raw = env->GetStringUTFChars(j_key, nullptr);
    std::string k(raw ? raw : "");
    env->ReleaseStringUTFChars(j_key, raw);
    LOGD("L2 native_get('%s') requested", k.c_str());

    static const std::map<std::string, std::string> map = {
        {"ro.serialno",              "SERIAL"},
        {"ro.boot.serialno",         "SERIAL"},
        {"ro.build.fingerprint",     "FINGERPRINT"},
        {"ro.bootimage.build.fingerprint", "FINGERPRINT"},
        {"ro.product.model",         "MODEL"},
        {"ro.product.brand",         "BRAND"},
        {"ro.product.manufacturer",  "MANUFACTURER"},
        {"ro.product.device",        "DEVICE"},
        {"ro.product.name",          "PRODUCT"},
        {"ro.product.board",         "BOARD"},
        {"ro.build.id",              "ID"},
        {"ro.build.display.id",      "DISPLAY"},
        {"ro.build.description",     "DESCRIPTION"},
        {"ro.build.version.release", "RELEASE"},
        {"ro.build.version.sdk",     "SDK_INT"},
        {"ro.build.version.security_patch", "SECURITY_PATCH"},
        {"ro.build.version.incremental",    "INCREMENTAL"},
        {"gsm.version.baseband",     "RADIO"},

        {"sys.boot_completed",       "SYS_BOOT_COMPLETED"},

        {"debug.force_rtl",          "DEBUG_FORCE_RTL"},
        {"persist.radio.multisim.config", "MULTISIM_CONFIG"},
        {"gsm.operator.numeric",     "GSM_OPERATOR_NUMERIC"},
        {"gsm.sim.operator.numeric", "GSM_OPERATOR_NUMERIC"},
        {"gsm.operator.alpha",       "GSM_OPERATOR_ALPHA"},
        {"gsm.sim.operator.alpha",   "GSM_OPERATOR_ALPHA"},
        {"gsm.operator.iso-country", "GSM_OPERATOR_ISO"},
        {"gsm.sim.operator.iso-country", "GSM_OPERATOR_ISO"},
        {"ro.build.characteristics", "BUILD_CHARACTERISTICS"},
        {"persist.sys.timezone",     "PERSIST_TIMEZONE"},
        {"ro.product.cpu.abi",       "CPU_ABI"},
        {"ro.product.cpu.abi2",      "CPU_ABI2"},
        {"ro.product.cpu.abilist",   "CPU_ABILIST"},
        {"ro.product.cpu.abilist64", "CPU_ABILIST64"},
        {"ro.product.cpu.abilist32", "CPU_ABILIST32"},
        {"dalvik.vm.heapgrowthlimit","DALVIK_HEAPGROWTHLIMIT"},
        {"ro.mediacodec.min_sample_rate", "MEDIACODEC_MIN_RATE"},
        {"ro.mediacodec.max_sample_rate", "MEDIACODEC_MAX_RATE"},
        {"ro.build.user",            "USER"},
        {"ro.build.host",            "HOST"},
        {"ro.build.tags",            "TAGS"},
        {"ro.build.type",            "TYPE"},
    };

    static const std::map<std::string, std::string> static_defaults = {
        {"gsm.operator.isroaming",         "false"},
        {"ro.zygote",                      "zygote64_32"},
        {"ro.hardware",                    "qcom"},
        {"ro.board.platform",              "sm8250"},
        {"ro.dalvik.vm.native.bridge",     "0"},
        {"ro.allow.mock.location",         "0"},
        {"dalvik.vm.isa.arm64.variant",    "generic"},
        {"dalvik.vm.isa.arm64.features",   "default"},
        {"dalvik.vm.isa.arm.variant",      "generic"},
        {"dalvik.vm.isa.arm.features",     "default"},
        {"dalvik.vm.heapsize",             "512m"},
        {"ro.build.version.preview_sdk",   "0"},
        {"persist.radio.multisim.config",  "ss"},
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
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(k.c_str(), buf) > 0) {
        LOGD("L2 LEAK  '%s' -> '%s' (unhooked, real value returned)", k.c_str(), buf);
        return env->NewStringUTF(buf);
    }
    LOGD("L2 MISS  '%s' -> default", k.c_str());
    return j_def;
}

static jstring (*orig_secure_get)(JNIEnv*, jclass, jobject, jstring) = nullptr;

static jstring hook_secure_get(JNIEnv* env, jclass c, jobject cr, jstring name) {
    if (name) {
        const char* raw = env->GetStringUTFChars(name, nullptr);
        std::string n(raw ? raw : "");
        env->ReleaseStringUTFChars(name, raw);
        LOGD("L3 Settings.Secure.getString('%s')", n.c_str());
        if (n == "android_id") {
            const std::string& aid = val("ANDROID_ID");
            if (!aid.empty()) {
                LOGD("L3 SPOOF android_id -> '%s'", aid.c_str());
                return env->NewStringUTF(aid.c_str());
            }
        }

        if (n == "bluetooth_address" || n == "bluetooth_name" ||
            n == "advertising_id"   || n == "install_non_market_apps") {
            LOGD("L3 LEAK  Settings.Secure '%s' queried (unhooked)", n.c_str());
        }
    }
    return orig_secure_get ? orig_secure_get(env, c, cr, name) : nullptr;
}

static void install_secure_hook(JNIEnv* env) {
    jclass c = env->FindClass("android/provider/Settings$Secure");
    if (!c) { env->ExceptionClear(); return; }
    JNINativeMethod m = {
        const_cast<char*>("getString"),
        const_cast<char*>("(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;"),
        reinterpret_cast<void*>(hook_secure_get)
    };
    env->RegisterNatives(c, &m, 1);
    env->ExceptionClear();
    env->DeleteLocalRef(c);
}

static void install_gaid_hook(JNIEnv* env) {
    jclass c = env->FindClass("com/google/android/gms/ads/identifier/AdvertisingIdClient$Info");
    if (!c) { env->ExceptionClear(); return; }

    env->DeleteLocalRef(c);
}

static jstring hook_wifi_mac(JNIEnv* env, jobject) {
    LOGD("L5 WifiInfo.getMacAddress -> 02:00:00:00:00:00 (spoofed)");
    return env->NewStringUTF("02:00:00:00:00:00");
}
static jstring hook_wifi_bssid(JNIEnv* env, jobject) {
    LOGD("L5 WifiInfo.getBSSID -> 02:00:00:00:00:00 (spoofed)");
    return env->NewStringUTF("02:00:00:00:00:00");
}

static void install_wifi_hook(JNIEnv* env) {
    jclass c = env->FindClass("android/net/wifi/WifiInfo");
    if (!c) { env->ExceptionClear(); return; }
    JNINativeMethod m[] = {
        {const_cast<char*>("getMacAddress"), const_cast<char*>("()Ljava/lang/String;"),
         reinterpret_cast<void*>(hook_wifi_mac)},
        {const_cast<char*>("getBSSID"), const_cast<char*>("()Ljava/lang/String;"),
         reinterpret_cast<void*>(hook_wifi_bssid)},
    };
    env->RegisterNatives(c, m, 2);
    env->ExceptionClear();
    env->DeleteLocalRef(c);
}

static jstring hook_null_str(JNIEnv*, jobject) { return nullptr; }

#ifdef TT_DEBUG

static jstring hook_tel_deviceId(JNIEnv*, jobject) { LOGD("L6 TelephonyManager.getDeviceId() -> null"); return nullptr; }
static jstring hook_tel_imei    (JNIEnv*, jobject) { LOGD("L6 TelephonyManager.getImei() -> null");     return nullptr; }
static jstring hook_tel_subId   (JNIEnv*, jobject) { LOGD("L6 TelephonyManager.getSubscriberId() -> null"); return nullptr; }
static jstring hook_tel_meid    (JNIEnv*, jobject) { LOGD("L6 TelephonyManager.getMeid() -> null");     return nullptr; }
#endif

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

#ifdef TT_DEBUG
static jint hook_prop_get_int(JNIEnv* env, jclass, jstring j_key, jint def) {
    jint out = def;
    const char* label = "LEAK";
    if (j_key) {
        const char* r = env->GetStringUTFChars(j_key, nullptr);
        std::string k(r ? r : "");
        env->ReleaseStringUTFChars(j_key, r);
        const auto& m = tt_int_spoof();
        auto it = m.find(k);
        if (it != m.end()) { out = it->second; label = "SPOOF"; }
        else if (tt_should_suppress_key(k)) { label = "SUPPRESS"; }
        LOGD("L7 SPI native_get_int('%s') def=%d -> %d [%s]", k.c_str(), def, out, label);
    }
    return out;
}
static jlong hook_prop_get_long(JNIEnv* env, jclass, jstring j_key, jlong def) {
    jlong out = def;
    const char* label = "LEAK";
    if (j_key) {
        const char* r = env->GetStringUTFChars(j_key, nullptr);
        std::string k(r ? r : "");
        env->ReleaseStringUTFChars(j_key, r);
        const auto& m = tt_long_spoof();
        auto it = m.find(k);
        if (it != m.end()) { out = it->second; label = "SPOOF"; }
        else if (tt_should_suppress_key(k)) { label = "SUPPRESS"; }
        LOGD("L7 SPL native_get_long('%s') def=%lld -> %lld [%s]",
             k.c_str(), (long long)def, (long long)out, label);
    }
    return out;
}
static jboolean hook_prop_get_bool(JNIEnv* env, jclass, jstring j_key, jboolean def) {
    jboolean out = def;
    const char* label = "LEAK";
    if (j_key) {
        const char* r = env->GetStringUTFChars(j_key, nullptr);
        std::string k(r ? r : "");
        env->ReleaseStringUTFChars(j_key, r);
        const auto& m = tt_bool_spoof();
        auto it = m.find(k);
        if (it != m.end()) { out = it->second; label = "SPOOF"; }
        else if (tt_should_suppress_key(k)) { label = "SUPPRESS"; }
        LOGD("L7 SPB native_get_boolean('%s') def=%d -> %d [%s]",
             k.c_str(), (int)def, (int)out, label);
    }
    return out;
}
static jstring hook_build_radio(JNIEnv* env, jclass) {
    LOGD("L7 Build.getRadioVersion() called [LEAK — returning empty]");
    return env->NewStringUTF("");
}
static void install_leak_sensors(JNIEnv* env) {

    {
        jclass sp = env->FindClass("android/os/SystemProperties");
        if (sp) {
            JNINativeMethod m[] = {
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
            env->RegisterNatives(sp, m, 3);
            env->ExceptionClear();
            env->DeleteLocalRef(sp);
            LOGD("L7 leak sensors installed on SystemProperties (int/long/bool)");
        } else env->ExceptionClear();
    }

    {
        jclass b = env->FindClass("android/os/Build");
        if (b) {
            JNINativeMethod m = {
                const_cast<char*>("getRadioVersion"),
                const_cast<char*>("()Ljava/lang/String;"),
                reinterpret_cast<void*>(hook_build_radio)};
            env->RegisterNatives(b, &m, 1);
            env->ExceptionClear();
            env->DeleteLocalRef(b);
            LOGD("L7 leak sensor installed on Build.getRadioVersion");
        } else env->ExceptionClear();
    }
}
#endif

static void install_telephony_hook(JNIEnv* env) {
    jclass c = env->FindClass("android/telephony/TelephonyManager");
    if (!c) { env->ExceptionClear(); return; }
#ifdef TT_DEBUG
    JNINativeMethod m[] = {
        {const_cast<char*>("getDeviceId"),     const_cast<char*>("()Ljava/lang/String;"), reinterpret_cast<void*>(hook_tel_deviceId)},
        {const_cast<char*>("getImei"),         const_cast<char*>("()Ljava/lang/String;"), reinterpret_cast<void*>(hook_tel_imei)},
        {const_cast<char*>("getSubscriberId"), const_cast<char*>("()Ljava/lang/String;"), reinterpret_cast<void*>(hook_tel_subId)},
        {const_cast<char*>("getMeid"),         const_cast<char*>("()Ljava/lang/String;"), reinterpret_cast<void*>(hook_tel_meid)},
    };
#else
    JNINativeMethod m[] = {
        {const_cast<char*>("getDeviceId"),     const_cast<char*>("()Ljava/lang/String;"), reinterpret_cast<void*>(hook_null_str)},
        {const_cast<char*>("getImei"),         const_cast<char*>("()Ljava/lang/String;"), reinterpret_cast<void*>(hook_null_str)},
        {const_cast<char*>("getSubscriberId"), const_cast<char*>("()Ljava/lang/String;"), reinterpret_cast<void*>(hook_null_str)},
        {const_cast<char*>("getMeid"),         const_cast<char*>("()Ljava/lang/String;"), reinterpret_cast<void*>(hook_null_str)},
    };
#endif
    env->RegisterNatives(c, m, 4);
    env->ExceptionClear();
    env->DeleteLocalRef(c);
}

static struct sigaction g_prev_sig[NSIG];
static std::string g_watchdog_pkg;
static long        g_load_time_ms = 0;

static volatile sig_atomic_t g_crash_count[NSIG] = {0};
static const int CRASH_LIMIT = 3;

static long tt_now_ms() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}
static const char* tt_sig_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGTERM: return "SIGTERM";
        case SIGPIPE: return "SIGPIPE";
        case SIGSYS:  return "SIGSYS";
        default:      return "?";
    }
}
static void tt_signal_handler(int sig, siginfo_t* info, void* ctx) {
    int n = 0;
    if (sig >= 0 && sig < NSIG) { g_crash_count[sig] = g_crash_count[sig] + 1; n = g_crash_count[sig]; }

    if (n <= CRASH_LIMIT) {
        long alive = tt_now_ms() - g_load_time_ms;

        LOGE("CRASH [%s] pkg=%s pid=%d signal=%d(%s) code=%d addr=%p sender=%d alive=%ldms hit=%d/%d",
             TT_VARIANT_TAG,
             g_watchdog_pkg.c_str(), getpid(),
             sig, tt_sig_name(sig),
             info ? info->si_code : 0,
             info ? info->si_addr : nullptr,
             info ? info->si_pid  : 0,
             alive, n, CRASH_LIMIT);
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
    g_watchdog_pkg  = pkg;
    g_load_time_ms  = tt_now_ms();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sa.sa_sigaction = tt_signal_handler;
    sigemptyset(&sa.sa_mask);

    static const int sigs[] = { SIGABRT, SIGFPE, SIGILL, SIGSYS };
    for (int s : sigs) {
        g_crash_count[s] = 0;
        sigaction(s, &sa, &g_prev_sig[s]);
    }
    LOGD("crash watchdog armed for %s (4 signals: ABRT/FPE/ILL/SYS, limit=%d)",
         pkg.c_str(), CRASH_LIMIT);
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

static const char* MOUNTDIR = "/data/adb/modules/ternak_tt/mount";

struct BindEntry { const char* src_rel; const char* dst; };
static const BindEntry BIND_ENTRIES[] = {
    {"system/build.prop",     "/system/build.prop"},
    {"vendor/build.prop",     "/vendor/build.prop"},
    {"odm/build.prop",        "/odm/build.prop"},
    {"product/build.prop",    "/product/build.prop"},
    {"system_ext/build.prop", "/system_ext/build.prop"},
    {"settings_secure.xml",   "/data/system/users/0/settings_secure.xml"},
};

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
    if (n != (ssize_t)sizeof(ok)) {
        LOGE("companion read ack failed");
        return;
    }
    LOGI("bind-mount via companion: %u ok (pid=%u)", ok, pid);
}

using openat_t = int (*)(int, const char*, int, ...);
static openat_t orig_openat = nullptr;

static bool is_sensitive_proc_path(const char* p) {
    if (!p) return false;
    if (!strstr(p, "/proc/")) return false;
    if (strstr(p, "/mountinfo")) return true;
    if (strstr(p, "/mounts"))    return true;
    if (strstr(p, "/maps"))      return true;
    return false;
}

static int hook_openat(int dirfd, const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (!orig_openat || !is_sensitive_proc_path(path)) {
        return orig_openat ? orig_openat(dirfd, path, flags, mode)
                           : ::openat(dirfd, path, flags, mode);
    }
    int real_fd = orig_openat(dirfd, path, flags, mode);
    if (real_fd < 0) return real_fd;

    std::string content;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(real_fd, buf, sizeof(buf))) > 0) content.append(buf, n);
    ::close(real_fd);

    std::string filtered;
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("ternak_tt") != std::string::npos) continue;
        if (line.find("ternak-tt") != std::string::npos) continue;
        filtered.append(line);
        filtered.push_back('\n');
    }

    int mfd = tt_memfd_create("clean", MFD_CLOEXEC);
    if (mfd < 0) return orig_openat(dirfd, path, flags, mode);
    if (!filtered.empty()) {
        ::write(mfd, filtered.data(), filtered.size());
    }
    ::lseek(mfd, 0, SEEK_SET);
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
    if (!api->pltHookCommit()) {
        LOGI("proc sanitizer: PLT commit false (best-effort skipped)");
    } else {
        LOGI("proc sanitizer installed (dev=%lu ino=%lu)",
             (unsigned long)dev, (unsigned long)ino);
    }
}

class TernakTT : public zygisk::ModuleBase {
public:
    void onLoad(Api* api, JNIEnv* env) override {
        api_ = api; env_ = env;
        LOGD("onLoad build=%s api=%p env=%p pid=%d uid=%d",
             TT_VARIANT_TAG, api, env, getpid(), getuid());
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

        // v1.1.3: Early bail-out BEFORE companion IPC for known non-target
        // root/system/shell processes. Motivation: on Android 15 SDK 35, even
        // a 1ms synchronous companion round-trip during preAppSpecialize can
        // race with ActivityThread.handleBindApplication when a
        // receiver-only process (e.g. BOOT_COMPLETED to Shizuku) is spawned,
        // causing NPE inside LoadedApk.makeApplicationInner because
        // mInstrumentation is not yet set. Skipping the IPC entirely for
        // these packages removes the race window. Root apps in this list
        // never need spoofing anyway (they see the real device, not the
        // TikTok/Grab persona).
        if (should_skip_early_v113(pkg)) {
            LOGD("early-skip pkg='%s' (root/system/shell manager) — v1.1.3",
                 pkg.c_str());
            unload();
            return;
        }

        int fd = api_->connectCompanion();
        LOGD("connectCompanion() -> fd=%d", fd);
        if (fd < 0) { LOGD("companion connect failed for pkg='%s'", pkg.c_str()); unload(); return; }

        // v1.1.3: 500ms hard cap on companion socket read/write so a wedged
        // companion daemon can never block preAppSpecialize indefinitely.
        {
            struct timeval tv;
            tv.tv_sec  = 0;
            tv.tv_usec = 500000;  // 500ms
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }

        uint8_t cmd = CMD_GET_IDENTITY;
        write(fd, &cmd, 1);
        uint16_t plen = (uint16_t)pkg.size();
        write(fd, &plen, sizeof(plen));
        if (plen) write(fd, pkg.data(), plen);

        uint32_t len = 0;
        if (read(fd, &len, sizeof(len)) != sizeof(len) || len > 65536) {
            close(fd); unload(); return;
        }
        if (len == 0) {

            LOGD("pkg='%s' not a target (companion), unloading", pkg.c_str());
            close(fd); unload(); return;
        }
        blob_.resize(len);
        size_t got = 0;
        while (got < len) {
            ssize_t n = read(fd, blob_.data() + got, len - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        close(fd);
        if (got != len) { unload(); return; }

        active_ = true;
        pkg_ = pkg;
        LOGI("target: %s (%u B) [%s]", pkg.c_str(), len, TT_VARIANT_TAG);
        LOGD("identity blob head='%.120s'",
             std::string(blob_.begin(), blob_.begin() + (len < 120 ? len : 120)).c_str());

        request_companion_mounts(api_);
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!active_) return;
        parse_blob();
        LOGD("parse_blob: %zu identity keys loaded", g_id.size());
#ifdef TT_DEBUG
        for (auto& kv : g_id) {
            LOGD("  [id] %s = %s", kv.first.c_str(), kv.second.c_str());
        }
#endif
        install_build_hook(env_);
        LOGD("L1 install_build_hook done");

        {
            jclass sp = env_->FindClass("android/os/SystemProperties");
            if (sp) {
                JNINativeMethod m = {
                    const_cast<char*>("native_get"),
                    const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
                    reinterpret_cast<void*>(hook_prop_get)};
                env_->RegisterNatives(sp, &m, 1);
                env_->ExceptionClear();
                env_->DeleteLocalRef(sp);
            } else env_->ExceptionClear();
        }
        install_secure_hook(env_);
        install_gaid_hook(env_);
        install_wifi_hook(env_);
        install_telephony_hook(env_);
#ifdef TT_DEBUG
        install_leak_sensors(env_);
#endif
        install_crash_watchdog(pkg_);

        // v1.1.1 L8: TimeZone + Locale default spoof (pure JNI, Path A).
        // Fixes the "Time zone" and "Locale (Region)" rows on device
        // fingerprint dashboards without needing lsplant. Clears the JVM's
        // cached TimeZone/Locale defaults for this target process by calling
        // setDefault() with spoofed values from the identity blob.
        {
            auto get_id = [](const char* k, const char* def) -> std::string {
                auto it = g_id.find(k);
                return (it != g_id.end() && !it->second.empty()) ? it->second : std::string(def);
            };
            const std::string tz_id    = get_id("TIMEZONE_ID",    "America/Los_Angeles");
            const std::string loc_lang = get_id("LOCALE_LANG",    "en");
            const std::string loc_ctry = get_id("LOCALE_COUNTRY", "US");

            // TimeZone.setDefault(TimeZone.getTimeZone(id))
            if (jclass TZ = env_->FindClass("java/util/TimeZone")) {
                jmethodID getTZ = env_->GetStaticMethodID(TZ, "getTimeZone",
                    "(Ljava/lang/String;)Ljava/util/TimeZone;");
                jmethodID setTZ = env_->GetStaticMethodID(TZ, "setDefault",
                    "(Ljava/util/TimeZone;)V");
                if (getTZ && setTZ) {
                    jstring id_s = env_->NewStringUTF(tz_id.c_str());
                    jobject tz_obj = env_->CallStaticObjectMethod(TZ, getTZ, id_s);
                    if (tz_obj && !env_->ExceptionCheck()) {
                        env_->CallStaticVoidMethod(TZ, setTZ, tz_obj);
                        LOGI("L8 TZ spoof: setDefault(%s)", tz_id.c_str());
                        env_->DeleteLocalRef(tz_obj);
                    }
                    env_->DeleteLocalRef(id_s);
                }
                env_->DeleteLocalRef(TZ);
            }
            if (env_->ExceptionCheck()) env_->ExceptionClear();

            // Locale.setDefault(new Locale(lang, country))
            if (jclass LC = env_->FindClass("java/util/Locale")) {
                jmethodID ctor = env_->GetMethodID(LC, "<init>",
                    "(Ljava/lang/String;Ljava/lang/String;)V");
                jmethodID setLC = env_->GetStaticMethodID(LC, "setDefault",
                    "(Ljava/util/Locale;)V");
                if (ctor && setLC) {
                    jstring lang_s = env_->NewStringUTF(loc_lang.c_str());
                    jstring ctry_s = env_->NewStringUTF(loc_ctry.c_str());
                    jobject loc_obj = env_->NewObject(LC, ctor, lang_s, ctry_s);
                    if (loc_obj && !env_->ExceptionCheck()) {
                        env_->CallStaticVoidMethod(LC, setLC, loc_obj);
                        LOGI("L8 Locale spoof: setDefault(%s_%s)",
                             loc_lang.c_str(), loc_ctry.c_str());
                        env_->DeleteLocalRef(loc_obj);
                    }
                    env_->DeleteLocalRef(lang_s);
                    env_->DeleteLocalRef(ctry_s);
                }
                env_->DeleteLocalRef(LC);
            }
            if (env_->ExceptionCheck()) env_->ExceptionClear();
        }

        // v1.1.0 Path B: lsplant-based Java method hooks (scaffold; no-op
        // unless built with -DTT_HAVE_LSPLANT=1 via fetch_lsplant.sh + CI).
        // v1.1.1: ident now populated from parsed identity blob (g_id).
        {
            if (ternak_tt::java_hooks::Init(env_)) {
                ternak_tt::java_hooks::InstallAll(env_, g_id);
            }
        }
    }

    void preServerSpecialize(ServerSpecializeArgs*) override { unload(); }

private:
    Api* api_ = nullptr;
    std::string pkg_;
    JNIEnv* env_ = nullptr;
    bool active_ = false;
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
            g_id[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
};

// v1.1.3 skip list — packages that must NEVER go through companion IPC.
// These are root/superuser managers, terminal apps, system apps, and Zygote
// sub-processes. Called only during preAppSpecialize, so cost is one
// std::string compare per app spawn.
static bool should_skip_early_v113(const std::string& pkg) {
    // Root / superuser / Zygisk managers
    static const char* const roots[] = {
        "me.weishu.kernelsu",
        "me.weishu.kernelsu.manager",
        "io.github.a13e300.ksuwebui",
        "com.rifsxd.ksunext",
        "com.topjohnwu.magisk",
        "io.github.vvb2060.magisk",
        "com.kernelsu.manager",
        "eu.chainfire.supersu",
        // Shizuku family (BOOT_COMPLETED race target on Android 15)
        "moe.shizuku.privileged.api",
        "moe.shizuku.manager",
        "rikka.shizuku.wrapper",
        // Riru / LSPosed / Xposed variants
        "moe.riru.core",
        "org.lsposed.manager",
        "de.robv.android.xposed.installer",
        // Termux family
        "com.termux",
        "com.termux.api",
        "com.termux.styling",
        "com.termux.boot",
        "com.termux.tasker",
        // Terminal / dev tools that often run as root
        "jackpal.androidterm",
        "com.spartacusrex.spartacuside",
        // Ourselves (defensive)
        "com.ternak.tt",
    };
    for (const char* r : roots) if (pkg == r) return true;

    // System packages — never spoofing targets
    if (pkg.rfind("android.", 0)          == 0) return true;
    if (pkg.rfind("com.android.", 0)      == 0) return true;
    if (pkg.rfind("com.google.android.gms", 0)      == 0) return true;
    if (pkg.rfind("com.google.android.gsf", 0)      == 0) return true;
    if (pkg.rfind("com.google.android.setupwizard", 0) == 0) return true;
    if (pkg.rfind("com.google.android.captiveportallogin", 0) == 0) return true;
    if (pkg.rfind("com.google.android.permission", 0) == 0) return true;
    if (pkg.rfind("com.google.android.packageinstaller", 0) == 0) return true;
    if (pkg == "system" || pkg == "system_server" || pkg == "android") return true;

    // Zygote / isolated sub-processes
    if (pkg.find(":zygote")            != std::string::npos) return true;
    if (pkg.find("_zygote")            != std::string::npos) return true;
    if (pkg.find(":isolated_process")  != std::string::npos) return true;
    if (pkg.find(":sandboxed_process") != std::string::npos) return true;
    if (pkg.find(":webview_service")   != std::string::npos) return true;

    return false;
}

REGISTER_ZYGISK_MODULE(TernakTT)

extern "C" void ternak_tt_companion(int client);
REGISTER_ZYGISK_COMPANION(ternak_tt_companion)
