
#ifndef TT_HOST_TEST
#include <jni.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/mount.h>
#include <sys/stat.h>
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
#else
#include "../tests/host_stub.h"
#include <vector>
#include <string>
#include <map>
#include <cstring>
#include <cstdlib>
#endif

#ifndef TT_HOST_TEST
enum : uint8_t {
    CMD_GET_IDENTITY = 2,
    CMD_DO_MOUNTS    = 3,
};
#endif

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
        {"TIMEZONE",              "Asia/Jakarta"},
        {"LOCALE",                "id-ID"},
        {"LOCALE_LANG",           "id"},
        {"LOCALE_COUNTRY",        "ID"},
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

enum class PropValueKind { Str, Int, Long, Bool };

struct PropRule {
    const char* key;
    bool is_prefix;
    const char* value_str;
};

static const PropRule g_prop_rules[] = {
    // Prefix rules (mask the whole MIUI / perf-monitor surface)
    {"ro.miui.",                          true, ""},
    {"persist.sys.miui_",                 true, ""},
    {"ro.mi.",                            true, ""},
    {"persist.sys.turbosched.",           true, ""},
    {"persist.sys.spc.",                  true, ""},
    {"persist.sys.perfdebug.",            true, ""},
    {"persist.sys.stability.",            true, ""},
    {"persist.sys.scout_",                true, ""},
    {"persist.sys.cachebuffer.",          true, ""},
    {"persist.sys.dynamicbuffer.",        true, ""},
    {"persist.sys.charlieprops.",         true, ""},
    {"ro.config.miui_",                   true, ""},
    {"ro.vendor.perf.scroll_opt",         true, ""},
    {"ro.dulquersalmaan22.",              true, ""},
    {"persist.sys.multi",                 true, ""},
    {"sys.displayfeature_",               true, ""},
    {"ro.df.effect.",                     true, ""},
    {"ro.vendor.df.effect.",              true, ""},
    {"persist.sys.vk_mode_",              true, ""},
    {"persist.reboot.coredump",           true, ""},

    // Exact rules
    {"ro.product.mod_device",             false, "DEVICE"}, // Maps to identity
    {"ro.gfx.driver.0",                   false, ""},
    {"ro.gfx.driver.1",                   false, ""},
    {"ro.gfx.driver_build_time",          false, "1704067200"},
    {"ro.product.vndk.version",           false, "VNDK_VERSION"}, // Maps to identity
    {"ro.vndk.version",                   false, "VNDK_VERSION"}, // Maps to identity
    {"ro.board.api_level",                false, "BOARD_API_LEVEL"}, // Maps to identity
    {"ro.board.first_api_level",          false, "BOARD_FIRST_API_LEVEL"}, // Maps to identity
    {"ro.vendor.api_level",               false, "BOARD_API_LEVEL"}, // Maps to identity
    {"ro.serialno",                       false, "SERIAL"}, // Maps to identity
    {"ro.boot.serialno",                  false, "SERIAL"}, // Maps to identity
    {"vendor.boot.serialno",              false, "SERIAL"}, // Maps to identity
    {"persist.sys.zygote.start_pid",      false, ""},
    {"sys.persist_screen_effect",         false, "0"},
};

static bool resolve_prop(const std::string& key, PropValueKind kind, std::string& out) {
    const PropRule* best_prefix = nullptr;
    size_t longest_prefix = 0;

    for (const auto& rule : g_prop_rules) {
        if (!rule.is_prefix) {
            if (key == rule.key) {
                // Exact match wins immediately
                std::string v = rule.value_str;
                // Check if it's an identity reference
                if (v == "DEVICE" || v == "SERIAL" || v == "VNDK_VERSION" || v == "BOARD_API_LEVEL" || v == "BOARD_FIRST_API_LEVEL") {
                    std::string mapped = val(v);
                    if (mapped.empty()) {
                        // Fallback derivation if identity.prop is old
                        std::string release = val("RELEASE");
                        if (v == "VNDK_VERSION" || v == "BOARD_FIRST_API_LEVEL") {
                            if (release == "13") mapped = "33";
                            else if (release == "14") mapped = "34";
                            else if (release == "15") mapped = "35";
                            else if (release == "16") mapped = "36";
                            else mapped = "35";
                        } else if (v == "BOARD_API_LEVEL") {
                            if (release == "13") mapped = "202305";
                            else if (release == "16") mapped = "202504";
                            else mapped = "202404";
                        }
                    }
                    v = mapped;
                }

                if (kind == PropValueKind::Str) out = v;
                else if (kind == PropValueKind::Int) {
                    out = v.empty() ? "0" : std::to_string(std::strtol(v.c_str(), nullptr, 10));
                }
                else if (kind == PropValueKind::Long) {
                    out = v.empty() ? "0" : std::to_string(std::strtoll(v.c_str(), nullptr, 10));
                }
                else if (kind == PropValueKind::Bool) {
                    bool b = (v == "1" || v == "true" || v == "yes");
                    out = b ? "1" : "0";
                }
                return true;
            }
        } else {
            size_t rule_len = std::strlen(rule.key);
            if (key.compare(0, rule_len, rule.key) == 0) {
                if (rule_len > longest_prefix) {
                    longest_prefix = rule_len;
                    best_prefix = &rule;
                }
            }
        }
    }

    if (best_prefix) {
        std::string v = best_prefix->value_str;
        if (kind == PropValueKind::Str) out = v;
        else if (kind == PropValueKind::Int) {
            out = v.empty() ? "0" : std::to_string(std::strtol(v.c_str(), nullptr, 10));
        }
        else if (kind == PropValueKind::Long) {
            out = v.empty() ? "0" : std::to_string(std::strtoll(v.c_str(), nullptr, 10));
        }
        else if (kind == PropValueKind::Bool) {
            bool b = (v == "1" || v == "true" || v == "yes");
            out = b ? "1" : "0";
        }
        return true;
    }

    return false;
}

#ifdef TT_HOST_TEST
// End of file for host test so we don't compile JNI hooks and Zygisk stuff
#else
static jstring hook_prop_get(JNIEnv* env, jclass, jstring j_key, jstring j_def) {
    if (!j_key) return j_def;
    const char* raw = env->GetStringUTFChars(j_key, nullptr);
    std::string k(raw ? raw : "");
    env->ReleaseStringUTFChars(j_key, raw);
    LOGD("L2 native_get('%s') requested", k.c_str());

    std::string resolved_val;
    if (resolve_prop(k, PropValueKind::Str, resolved_val)) {
        LOGD("L2 SPOOF '%s' -> '%s'", k.c_str(), resolved_val.c_str());
        return env->NewStringUTF(resolved_val.c_str());
    }

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
        {"persist.sys.timezone",     "TIMEZONE"},
        {"persist.sys.locale",       "LOCALE"},
        {"ro.product.locale",        "LOCALE"},
        {"ro.product.locale.language", "LOCALE_LANG"},
        {"ro.product.locale.region", "LOCALE_COUNTRY"},
        {"ro.soc.manufacturer",      "SOC_MANUFACTURER"},
        {"ro.soc.model",             "SOC_MODEL"},
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

        std::string resolved_val;
        if (resolve_prop(k, PropValueKind::Int, resolved_val)) {
            out = (jint)std::strtol(resolved_val.c_str(), nullptr, 10);
            label = "SPOOF";
        } else {
            const auto& m = tt_int_spoof();
            auto it = m.find(k);
            if (it != m.end()) {
                out = it->second;
                label = "SPOOF";
            } else if (tt_should_suppress_key(k)) {
                label = "SUPPRESS";   // keep def, do not read real prop
            } else {
                char buf[PROP_VALUE_MAX] = {0};
                if (__system_property_get(k.c_str(), buf) > 0) {
                    char* end = nullptr;
                    long v = std::strtol(buf, &end, 10);
                    if (end != buf) out = (jint)v;  // only if numeric
                }
            }
        }
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

        std::string resolved_val;
        if (resolve_prop(k, PropValueKind::Long, resolved_val)) {
            out = (jlong)std::strtoll(resolved_val.c_str(), nullptr, 10);
            label = "SPOOF";
        } else {
            const auto& m = tt_long_spoof();
            auto it = m.find(k);
            if (it != m.end()) {
                out = it->second;
                label = "SPOOF";
            } else if (tt_should_suppress_key(k)) {
                label = "SUPPRESS";   // keep def, do not read real prop
            } else {
                char buf[PROP_VALUE_MAX] = {0};
                if (__system_property_get(k.c_str(), buf) > 0) {
                    out = std::strtoll(buf, nullptr, 10);
                }
            }
        }
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

        std::string resolved_val;
        if (resolve_prop(k, PropValueKind::Bool, resolved_val)) {
            out = (resolved_val == "1") ? JNI_TRUE : JNI_FALSE;
            label = "SPOOF";
        } else {
            const auto& m = tt_bool_spoof();
            auto it = m.find(k);
            if (it != m.end()) {
                out = it->second;
                label = "SPOOF";
            } else if (tt_should_suppress_key(k)) {
                label = "SUPPRESS";   // keep def, do not read real prop
            } else {
                char buf[PROP_VALUE_MAX] = {0};
                if (__system_property_get(k.c_str(), buf) > 0) {
                    if (!strcmp(buf, "1") || !strcmp(buf, "true") || !strcmp(buf, "y") || !strcmp(buf, "yes") || !strcmp(buf, "on")) out = JNI_TRUE;
                    else if (!strcmp(buf, "0") || !strcmp(buf, "false") || !strcmp(buf, "n") || !strcmp(buf, "no") || !strcmp(buf, "off")) out = JNI_FALSE;
                }
            }
        }
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
    if (sig >= 0 && sig < NSIG) {
        n = g_crash_count[sig] + 1;
        g_crash_count[sig] = n;
    }

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
            // API 31+ fields; set_str no-ops via ExceptionClear if absent.
            {"SOC_MANUFACTURER","SOC_MANUFACTURER"}, {"SOC_MODEL","SOC_MODEL"},
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

// Apply the persona timezone + locale to *this* app process only. These are
// COPG's "stealth, system-side" locale/timezone: we shift the process defaults
// (java.util.TimeZone / java.util.Locale) plus the C-library TZ, so the app and
// its native code read the persona region without any device-wide change.
static void install_locale_hook(JNIEnv* env) {
    const std::string tz = val("TIMEZONE");
    if (!tz.empty()) {
        setenv("TZ", tz.c_str(), 1);
        tzset();
        jclass tzc = env->FindClass("java/util/TimeZone");
        if (tzc) {
            jmethodID get = env->GetStaticMethodID(
                tzc, "getTimeZone", "(Ljava/lang/String;)Ljava/util/TimeZone;");
            jmethodID setDef = env->GetStaticMethodID(
                tzc, "setDefault", "(Ljava/util/TimeZone;)V");
            if (get && setDef) {
                jstring js = env->NewStringUTF(tz.c_str());
                jobject tzo = env->CallStaticObjectMethod(tzc, get, js);
                if (tzo) {
                    env->CallStaticVoidMethod(tzc, setDef, tzo);
                    env->DeleteLocalRef(tzo);
                }
                if (js) env->DeleteLocalRef(js);
            }
            env->ExceptionClear();
            env->DeleteLocalRef(tzc);
            LOGD("L3 timezone default set to '%s'", tz.c_str());
        } else env->ExceptionClear();
    }

    const std::string tag = val("LOCALE");   // BCP-47, e.g. "id-ID"
    if (!tag.empty()) {
        jclass lc = env->FindClass("java/util/Locale");
        if (lc) {
            jmethodID forTag = env->GetStaticMethodID(
                lc, "forLanguageTag", "(Ljava/lang/String;)Ljava/util/Locale;");
            jmethodID setDef = env->GetStaticMethodID(
                lc, "setDefault", "(Ljava/util/Locale;)V");
            if (forTag && setDef) {
                jstring js = env->NewStringUTF(tag.c_str());
                jobject lo = env->CallStaticObjectMethod(lc, forTag, js);
                if (lo) {
                    env->CallStaticVoidMethod(lc, setDef, lo);
                    env->DeleteLocalRef(lo);
                }
                if (js) env->DeleteLocalRef(js);
            }
            env->ExceptionClear();
            env->DeleteLocalRef(lc);
            LOGD("L3 locale default set to '%s'", tag.c_str());
        } else env->ExceptionClear();
    }
}

// Opt-in fake uptime. We re-implement the three SystemClock readers the
// platform backs with clock_gettime and add a fixed offset, so an app that
// distrusts a freshly-reset device sees a phone that has been up for a while.
// The offset is constant, so all three stay monotonic and their deltas (what
// timers/animations actually use) are unchanged. currentTimeMillis is left
// alone on purpose — shifting wall-clock time breaks TLS/cert validation.
static jlong g_uptime_offset_ms = 0;

// @CriticalNative handlers take no JNIEnv/jclass
static jlong tt_sysclock_uptime_millis() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (jlong)(ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL) + g_uptime_offset_ms;
}
static jlong tt_sysclock_elapsed_realtime() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (jlong)(ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL) + g_uptime_offset_ms;
}
static jlong tt_sysclock_elapsed_realtime_nanos() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (jlong)(ts.tv_sec * 1000000000LL + ts.tv_nsec)
         + g_uptime_offset_ms * 1000000LL;
}

static void install_uptime_hook(JNIEnv* env) {
    const std::string v = val("FAKE_UPTIME_MS");
    if (v.empty()) return;                       // opt-in: off unless persona sets it
    long long off = std::strtoll(v.c_str(), nullptr, 10);
    constexpr long long MAX_UPTIME_MS = 10LL * 365 * 24 * 60 * 60 * 1000; // ~10y
    if (off <= 0) return;
    if (off > MAX_UPTIME_MS) off = MAX_UPTIME_MS;
    g_uptime_offset_ms = (jlong)off;

    jclass sc = env->FindClass("android/os/SystemClock");
    if (!sc) { env->ExceptionClear(); return; }
    JNINativeMethod m[] = {
        {const_cast<char*>("uptimeMillis"),        const_cast<char*>("()J"),
         reinterpret_cast<void*>(tt_sysclock_uptime_millis)},
        {const_cast<char*>("elapsedRealtime"),     const_cast<char*>("()J"),
         reinterpret_cast<void*>(tt_sysclock_elapsed_realtime)},
        {const_cast<char*>("elapsedRealtimeNanos"), const_cast<char*>("()J"),
         reinterpret_cast<void*>(tt_sysclock_elapsed_realtime_nanos)},
    };
    env->RegisterNatives(sc, m, 3);
    env->ExceptionClear();
    env->DeleteLocalRef(sc);
    LOGI("fake uptime enabled: +%lld ms", (long long)off);
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
    if (n != (ssize_t)sizeof(ok)) {
        LOGE("companion read ack failed");
        return;
    }
    LOGI("bind-mount via companion: %u ok (pid=%u)", ok, pid);
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

        int fd = api_->connectCompanion();
        LOGD("connectCompanion() -> fd=%d", fd);
        if (fd < 0) { LOGD("companion connect failed for pkg='%s'", pkg.c_str()); unload(); return; }

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

        install_locale_hook(env_);
        install_uptime_hook(env_);
#ifdef TT_DEBUG
        install_leak_sensors(env_);
#endif
        install_crash_watchdog(pkg_);

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

REGISTER_ZYGISK_MODULE(TernakTT)

extern "C" void ternak_tt_companion(int client);
REGISTER_ZYGISK_COMPANION(ternak_tt_companion)
#endif
