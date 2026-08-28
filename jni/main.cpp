
#include <jni.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <sys/system_properties.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <android/log.h>
#include <string>
#include <map>
#include <vector>
#include <utility>
#include <sstream>
#include <signal.h>
#include <ctime>
#include <thread>
#include "zygisk.hpp"
#include "config.hpp"
#include "sbx_lsplant.hpp"
#include "sbx_native_read.hpp"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
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

#define LOG_TAG "SandboxID"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

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
        // Marketname prefixed variants — Build.java reads ro.product.marketname
        // through the same per-part fallback chain as model/brand (AOSP main).
        {"ro.product.system.marketname",    "MARKETNAME"},
        {"ro.product.odm.marketname",       "MARKETNAME"},
        {"ro.product.product.marketname",   "MARKETNAME"},
        {"ro.product.board",                "BOARD"},
        {"ro.hardware",                     "HARDWARE"},
        {"ro.board.platform",               "BOARD_PLATFORM"},
        {"ro.soc.manufacturer",             "SOC_MANUFACTURER"},
        {"ro.soc.model",                    "SOC_MODEL"},
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
        {"gsm.sim.state",                   "GSM_SIM_STATE"},
        {"gsm.sim.state.ril",               "GSM_SIM_STATE"},
        {"ro.build.characteristics",        "BUILD_CHARACTERISTICS"},
        {"persist.sys.timezone",            "PERSIST_TIMEZONE"},
        // ABI props — route to the same identity keys L1 Build.* hook uses,
        // so native getprop() and Java Build.SUPPORTED_ABIS both see the same
        // CSV value (identity keys populated by sandboxid::derive_identity()).
        {"ro.product.cpu.abi",              "CPU_ABI"},
        {"ro.product.cpu.abi2",             "CPU_ABI2"},
        {"ro.product.cpu.abilist",          "SUPPORTED_ABIS"},
        {"ro.product.cpu.abilist64",        "SUPPORTED_64_BIT_ABIS"},
        {"ro.product.cpu.abilist32",        "SUPPORTED_32_BIT_ABIS"},
        {"dalvik.vm.heapgrowthlimit",       "DALVIK_HEAPGROWTHLIMIT"},
        {"ro.mediacodec.min_sample_rate",   "MEDIACODEC_MIN_RATE"},
        {"ro.mediacodec.max_sample_rate",   "MEDIACODEC_MAX_RATE"},
        {"ro.build.user",                   "USER"},
        {"ro.build.host",                   "HOST"},
        {"ro.build.tags",                   "TAGS"},
        {"ro.build.type",                   "TYPE"},
        // Build date — Build.TIME = getLong("ro.build.date.utc") * 1000
        // (AOSP Build.java main). Fingerprint SDKs cross-check these against
        // the persona fingerprint; real device values would leak.
        {"ro.build.date.utc",               "BUILD_TIME_UTC"},
        {"ro.build.date",                   "BUILD_DATE"},
        // ro.build.flavor ("<product>-<type>") — Build.FLAVOR was removed from
        // the SDK but the property still exists on production builds and is
        // read directly by fingerprint SDKs.
        {"ro.build.flavor",                 "FLAVOR"},

        {"ro.boot.vbmeta.digest",           "VBMETA_DIGEST"},
        {"ro.build.product",                     "DEVICE"},
        {"ro.build.version.release_or_codename", "RELEASE"},
        {"ro.vendor.build.security_patch",       "SECURITY_PATCH"},

        {"ro.system.build.fingerprint",     "FINGERPRINT"},
        {"ro.vendor.build.fingerprint",     "FINGERPRINT"},
        {"ro.odm.build.fingerprint",        "FINGERPRINT"},
        {"ro.product.build.fingerprint",    "FINGERPRINT"},
        {"ro.system_ext.build.fingerprint", "FINGERPRINT"},
        {"ro.vendor_dlkm.build.fingerprint", "FINGERPRINT"},
        {"ro.odm_dlkm.build.fingerprint",   "FINGERPRINT"},

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

        // Phase 3 additions (validated 2026-08 against AOSP Build.java main):
        //   Build.SKU, Build.ODM_SKU, Build.VERSION.BASE_OS,
        //   Build.VERSION.PREVIEW_SDK_INT, Build.VERSION.MEDIA_PERFORMANCE_CLASS
        // All read via SystemProperties.get / getInt at Build class init.
        {"ro.boot.hardware.sku",                     "SKU"},
        {"ro.boot.product.hardware.sku",             "ODM_SKU"},
        {"ro.build.version.base_os",                 "BASE_OS"},
        {"ro.build.version.preview_sdk",             "PREVIEW_SDK_INT"},
        {"ro.build.version.preview_sdk_fingerprint", "PREVIEW_SDK_FINGERPRINT"},
        {"ro.odm.build.media_performance_class",     "MEDIA_PERFORMANCE_CLASS"},
    };
    return m;
}

static bool spoof_prop_value(const std::string& k, std::string& out) {
    const auto& map = prop_to_identity_map();
    auto it = map.find(k);
    if (it != map.end()) {
        const std::string& v = val(it->second);
        if (!v.empty()) { out = v; return true; }
    }
    for (size_t i = 0; i < sandboxid::STATIC_PROP_DEFAULTS_N; ++i) {
        if (k == sandboxid::STATIC_PROP_DEFAULTS[i].k) {
            out = sandboxid::STATIC_PROP_DEFAULTS[i].v;
            return true;
        }
    }
    return false;
}

static inline bool sbx_prop_hidden(const char* name) {
    return name && sbxnr::should_hide_prop(name);
}

static jstring hook_prop_get(JNIEnv* env, jclass clazz, jstring j_key, jstring j_def) {
    if (!j_key) return j_def;

    const char* raw = env->GetStringUTFChars(j_key, nullptr);
    if (!raw) { if (env->ExceptionCheck()) env->ExceptionClear(); return j_def; }
    std::string k(raw);
    env->ReleaseStringUTFChars(j_key, raw);
    LOGD("L2 native_get('%s')", k.c_str());

    std::string v;
    if (spoof_prop_value(k, v)) {
        LOGD("L2 SPOOF '%s' -> '%s'", k.c_str(), v.c_str());
        return env->NewStringUTF(v.c_str());
    }

    if (sbx_prop_hidden(k.c_str())) {
        LOGD("L2 HIDE '%s' (report absent)", k.c_str());
        return j_def;
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
    // String-keyed SystemProperties natives are @FastNative (AOSP main), so
    // the (JNIEnv*, jclass, …) hook signature is the correct calling
    // convention. Handle-based natives are @CriticalNative and must NOT be
    // hooked with this signature.
    bool ok = api->hookJniNativeMethods(env, "android/os/SystemProperties", &m, 1);
    if (!ok || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("L2: native_get hook FAILED (api_rc=%d) — prop spoofing off for this process",
             (int)ok);
    }
    orig_native_get = reinterpret_cast<jstring (*)(JNIEnv*, jclass, jstring, jstring)>(m.fnPtr);
    if (orig_native_get)
        LOGD("L2 native_get hooked (orig=%p)", reinterpret_cast<void*>(orig_native_get));
    else
        LOGE("L2 native_get hook did not bind (method missing?)");
}

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

        // Phase 3 (2026-08): pin production values so native_get_int() returns
        // the canonical Pixel-stable values even when Build.VERSION.PREVIEW_SDK_INT
        // is read directly via SystemProperties.getInt (bypassing L1 field spoof).
        //   ref: AOSP Build.java main branch (getInt call sites)
        {"ro.build.version.preview_sdk",          0},
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

// Strict base-10 integer parse of an identity value ("34" yes, "34x"/"" no).
// Used so keys that AOSP initializes via SystemProperties.getInt/getLong
// (ro.build.version.sdk, ro.build.version.preview_sdk,
// ro.odm.build.media_performance_class, ro.build.date.utc) return the persona
// value through the typed getters too — not just the String native_get path.
static bool sbx_parse_ll(const std::string& v, long long& out) {
    if (v.empty()) return false;
    errno = 0;
    const char* s = v.c_str();
    // Skip a single leading +/- sign.
    if (*s == '+' || *s == '-') ++s;
    if (!*s) return false;
    char* end = nullptr;
    long long n = std::strtoll(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0' || errno == ERANGE) return false;
    out = n;
    return true;
}

static jint hook_prop_get_int(JNIEnv* env, jclass clazz, jstring j_key, jint def) {
    if (!j_key) return def;
    const char* r = env->GetStringUTFChars(j_key, nullptr);
    if (!r) { if (env->ExceptionCheck()) env->ExceptionClear(); return def; }
    std::string k(r);
    env->ReleaseStringUTFChars(j_key, r);

    // Identity-first: the persona value must win over the real device value
    // even when the caller uses the typed getter. Non-numeric identity values
    // (model, fingerprint, …) fail the parse and fall through unharmed.
    std::string v;
    if (spoof_prop_value(k, v)) {
        long long n = 0;
        if (sbx_parse_ll(v, n)) { LOGD("L7 SPI(id) '%s' -> %d", k.c_str(), (int)n); return (jint)n; }
    }

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

    // Identity-first (same rationale as hook_prop_get_int). ro.build.date.utc
    // lands here and keeps Build.TIME consistent with the persona fingerprint.
    std::string v;
    if (spoof_prop_value(k, v)) {
        long long n = 0;
        if (sbx_parse_ll(v, n)) { LOGD("L7 SPL(id) '%s' -> %lld", k.c_str(), (long long)n); return (jlong)n; }
    }

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
    // Same @FastNative contract as native_get above.
    bool ok = api->hookJniNativeMethods(env, "android/os/SystemProperties", m, 3);
    if (!ok || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("L7: leak-sensor hooks FAILED (api_rc=%d) — typed getters unspoofed",
             (int)ok);
    }
    orig_get_int  = reinterpret_cast<jint (*)(JNIEnv*, jclass, jstring, jint)>(m[0].fnPtr);
    orig_get_long = reinterpret_cast<jlong (*)(JNIEnv*, jclass, jstring, jlong)>(m[1].fnPtr);
    orig_get_bool = reinterpret_cast<jboolean (*)(JNIEnv*, jclass, jstring, jboolean)>(m[2].fnPtr);
    LOGD("L7 leak sensors installed (int/long/bool)");
}

#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#ifndef CLOCK_BOOTTIME_ALARM
#define CLOCK_BOOTTIME_ALARM 9
#endif

static int (*orig_clock_gettime)(clockid_t, struct timespec*) = nullptr;
static int64_t g_boot_off_sec = 0;

static int sbx_hooked_clock_gettime(clockid_t clk, struct timespec* ts) {
    int r = orig_clock_gettime ? orig_clock_gettime(clk, ts) : clock_gettime(clk, ts);
    if (r == 0 && ts && (clk == CLOCK_BOOTTIME || clk == CLOCK_BOOTTIME_ALARM))
        ts->tv_sec += g_boot_off_sec;
    return r;
}

static bool sbx_lib_dev_inode(const char* suffix, dev_t* out_dev, ino_t* out_ino) {
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) return false;
    char line[512];
    size_t sl = strlen(suffix);
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char* path = strchr(line, '/');
        if (!path) continue;
        size_t pl = strlen(path);
        if (pl && path[pl - 1] == '\n') path[--pl] = '\0';
        if (pl >= sl && strcmp(path + pl - sl, suffix) == 0) {
            struct stat st;
            if (stat(path, &st) == 0) { *out_dev = st.st_dev; *out_ino = st.st_ino; found = true; }
            break;
        }
    }
    fclose(f);
    return found;
}

static void install_uptime_hook(Api* api, JNIEnv*  ) {
    const std::string& us = val("UPTIME_SECONDS");
    if (us.empty()) return;
    char* end = nullptr;
    long long secs = std::strtoll(us.c_str(), &end, 10);
    if (end == us.c_str() || secs <= 0) return;
    g_boot_off_sec = (int64_t)secs;

    // Chokepoint libraries known to call clock_gettime(CLOCK_BOOTTIME) directly.
    // References:
    //   - AOSP frameworks/native/libs/utils/SystemClock.cpp  (libutils)
    //   - AOSP system/libbase/chrono_utils.cpp               (libbase / boot_clock)
    //   - AOSP system/core/libcutils/                        (libcutils android_get_uptime)
    //   - frameworks/base/core/jni/android_os_SystemClock.cpp (libandroid_runtime)
    // Adding libbase.so + libcutils.so closes gaps where system apps link them directly
    // and bypass libutils/libandroid_runtime. Missing libs are silently skipped by
    // sbx_lib_dev_inode() returning false.
    static const char* const kLibs[] = {
        "/libutils.so",
        "/libandroid_runtime.so",
        "/libbase.so",     // Added Phase 3 — android::base::boot_clock
        "/libcutils.so",   // Added Phase 3 — android_get_uptime()
    };
    int registered = 0;
    int found      = 0;
    for (size_t i = 0; i < sizeof(kLibs) / sizeof(kLibs[0]); ++i) {
        dev_t dev = 0; ino_t ino = 0;
        if (!sbx_lib_dev_inode(kLibs[i], &dev, &ino)) continue;
        ++found;
        api->pltHookRegister(dev, ino, "clock_gettime",
                             reinterpret_cast<void*>(sbx_hooked_clock_gettime),
                             reinterpret_cast<void**>(&orig_clock_gettime));
        ++registered;
    }
    if (registered == 0) {
        LOGW("L8: chokepoint lib tak ketemu (scanned %zu) — uptime tak dispoof",
             sizeof(kLibs) / sizeof(kLibs[0]));
        return;
    }
    if (!api->pltHookCommit()) {
        orig_clock_gettime = nullptr;
        LOGW("L8: pltHookCommit gagal (%d/%zu libs registered) — uptime tak dispoof (jam asli)",
             registered, sizeof(kLibs) / sizeof(kLibs[0]));
        return;
    }
    if (orig_clock_gettime == nullptr) {
        LOGW("L8: commit OK tapi clock_gettime tak ter-hook (orig=null, %d libs) — uptime tak dispoof",
             registered);
        return;
    }
    LOGD("L8 boottime PLT hook aktif (+%llds, %d/%d lib mapped, %zu scanned) orig=%p",
         (long long)secs, registered, found, sizeof(kLibs) / sizeof(kLibs[0]),
         reinterpret_cast<void*>(orig_clock_gettime));
}

#ifndef O_TMPFILE
#define O_TMPFILE 0
#endif

typedef int   (*sbx_open_fn)(const char*, int, ...);
typedef int   (*sbx_openat_fn)(int, const char*, int, ...);
typedef FILE* (*sbx_fopen_fn)(const char*, const char*);
typedef int   (*sbx_spg_fn)(const char*, char*);
typedef int   (*sbx_spr_fn)(const void*, char*, char*);
typedef void  (*sbx_prop_cb)(void*, const char*, const char*, uint32_t);
typedef void  (*sbx_sprcb_fn)(const void*, sbx_prop_cb, void*);

static sbx_open_fn   orig_open   = nullptr;
static sbx_openat_fn orig_openat = nullptr;
static sbx_fopen_fn  orig_fopen  = nullptr;
static sbx_spg_fn    orig_spg    = nullptr;
static sbx_spr_fn    orig_spr    = nullptr;
static sbx_sprcb_fn  orig_sprcb  = nullptr;

static bool        g_nr_active    = false;
static std::string g_boot_id;
static std::string g_wifi_mac;
static std::string g_proc_version;
static std::string g_cpu_repl;
static int         g_ram_gb    = 0;
static int         g_cpu_action = sbxnr::CPU_NONE;

static void sbx_fill_prop(char* value, const std::string& v) {
    size_t n = v.size();
    if (n > PROP_VALUE_MAX - 1) n = PROP_VALUE_MAX - 1;
    memcpy(value, v.data(), n);
    value[n] = '\0';
}

static int sbx_spg(const char* name, char* value) {
    if (g_nr_active && name && value) {
        if (sbx_prop_hidden(name)) { value[0] = '\0'; return 0; }
        std::string v;
        if (spoof_prop_value(name, v)) { sbx_fill_prop(value, v); return (int)v.size(); }
    }
    if (orig_spg) return orig_spg(name, value);
    if (value) value[0] = '\0';
    return 0;
}

static int sbx_spr(const void* pi, char* name, char* value) {
    int r = orig_spr ? orig_spr(pi, name, value) : -1;
    if (r >= 0 && g_nr_active && name && value) {
        if (sbx_prop_hidden(name)) { value[0] = '\0'; return 0; }
        std::string v;
        if (spoof_prop_value(name, v)) { sbx_fill_prop(value, v); return (int)v.size(); }
    }
    return r;
}

struct SbxCbCtx { sbx_prop_cb cb; void* cookie; };
static void sbx_cb_tramp(void* cookie, const char* name, const char* value, uint32_t serial) {
    SbxCbCtx* c = static_cast<SbxCbCtx*>(cookie);
    std::string v;
    if (g_nr_active && name && sbx_prop_hidden(name))
        return;
    else if (g_nr_active && name && spoof_prop_value(name, v))
        c->cb(c->cookie, name, v.c_str(), serial);
    else
        c->cb(c->cookie, name, value, serial);
}
static void sbx_sprcb(const void* pi, sbx_prop_cb cb, void* cookie) {
    if (!orig_sprcb) return;
    if (!cb) { orig_sprcb(pi, cb, cookie); return; }
    SbxCbCtx ctx{cb, cookie};
    orig_sprcb(pi, sbx_cb_tramp, &ctx);
}

static int sbx_make_memfd(const std::string& content) {
#ifdef __NR_memfd_create

    int fd = (int)syscall(__NR_memfd_create, "", (unsigned)MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (!sandboxid::write_full(fd, content.data(), content.size())) { ::close(fd); return -1; }
    if (::lseek(fd, 0, SEEK_SET) != 0) { ::close(fd); return -1; }
    return fd;
#else
    (void)content;
    return -1;
#endif
}

static std::string sbx_read_real(const char* path) {
    if (!orig_openat) return "";
    int fd = orig_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    std::string data;
    char buf[4096];
    for (;;) {
        ssize_t r = ::read(fd, buf, sizeof(buf));
        if (r > 0) { data.append(buf, (size_t)r); continue; }
        if (r < 0 && errno == EINTR) continue;
        break;
    }
    ::close(fd);
    return data;
}

static bool sbx_build_content(sbxnr::Kind kind, std::string& out) {
    switch (kind) {
        case sbxnr::BOOTID:  out = g_boot_id;      out.push_back('\n'); return true;
        case sbxnr::MAC:     out = g_wifi_mac;     out.push_back('\n'); return true;
        case sbxnr::VERSION: out = g_proc_version; out.push_back('\n'); return true;
        case sbxnr::SELINUX_ENFORCE:

            out = sbxnr::selinux_enforce_content();
            return true;
        case sbxnr::MEMINFO: {
            std::string real = sbx_read_real("/proc/meminfo");
            if (real.empty()) return false;
            out = sbxnr::patch_meminfo(real, g_ram_gb);
            return true;
        }
        case sbxnr::CPUINFO: {
            if (g_cpu_action == sbxnr::CPU_NONE) return false;
            std::string real = sbx_read_real("/proc/cpuinfo");
            if (real.empty()) return false;
            return sbxnr::patch_cpuinfo(real, g_cpu_action, g_cpu_repl, out);
        }
        default: return false;
    }
}

static int sbx_spoof_fd(const char* path) {
    sbxnr::Kind kind = sbxnr::classify(path);
    if (kind == sbxnr::NONE) return -1;
    std::string content;
    if (!sbx_build_content(kind, content)) return -1;
    int fd = sbx_make_memfd(content);
    if (fd >= 0) LOGD("L9 redirect '%s' -> memfd (%zu B)", path, content.size());
    return fd;
}

static inline bool sbx_is_pure_read(int flags) {
    return (flags & O_ACCMODE) == O_RDONLY && !(flags & (O_CREAT | O_TMPFILE));
}

static int sbx_openat(int dirfd, const char* pathname, int flags, ...) {
    mode_t mode = 0;
    bool has_mode = (flags & (O_CREAT | O_TMPFILE)) != 0;
    if (has_mode) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }

    if (g_nr_active && pathname && sbx_is_pure_read(flags)) {
        int fd = sbx_spoof_fd(pathname);
        if (fd >= 0) return fd;
    }
    if (orig_openat)
        return has_mode ? orig_openat(dirfd, pathname, flags, mode)
                        : orig_openat(dirfd, pathname, flags);
    return (int)syscall(__NR_openat, dirfd, pathname, flags, mode);
}

static int sbx_open(const char* pathname, int flags, ...) {
    mode_t mode = 0;
    bool has_mode = (flags & (O_CREAT | O_TMPFILE)) != 0;
    if (has_mode) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }

    if (g_nr_active && pathname && sbx_is_pure_read(flags)) {
        int fd = sbx_spoof_fd(pathname);
        if (fd >= 0) return fd;
    }
    if (orig_open)
        return has_mode ? orig_open(pathname, flags, mode) : orig_open(pathname, flags);
    if (orig_openat)
        return has_mode ? orig_openat(AT_FDCWD, pathname, flags, mode)
                        : orig_openat(AT_FDCWD, pathname, flags);
    return (int)syscall(__NR_openat, AT_FDCWD, pathname, flags, mode);
}

// Minimal fopen(3) mode -> open(2) flags mapping for the sbx_fopen fallback.
static int sbx_fopen_flags(const char* mode) {
    bool plus = mode && std::strchr(mode, '+');
    switch (mode ? mode[0] : 'r') {
        case 'w': return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
        case 'a': return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
        default:  return plus ? O_RDWR : O_RDONLY;   // 'r' and anything unknown
    }
}

static FILE* sbx_fopen(const char* path, const char* mode) {

    if (g_nr_active && path && mode && mode[0] == 'r' && !strchr(mode, '+')) {
        int fd = sbx_spoof_fd(path);
        if (fd >= 0) {
            FILE* fp = fdopen(fd, "r");
            if (fp) return fp;
            ::close(fd);
        }
    }
    if (orig_fopen) return orig_fopen(path, mode);

    // Fallback: the fopen PLT slot was not resolved for this process (partial
    // L9 commit) but open/openat may be. Route through them + fdopen instead
    // of failing with ENOSYS — apps legitimately use fopen() for reads AND
    // writes, and a hard failure here would break them.
    int fl = sbx_fopen_flags(mode);
    int fd = -1;
    if (orig_openat)      fd = orig_openat(AT_FDCWD, path, fl, 0666);
    else if (orig_open)   fd = orig_open(path, fl, 0666);
    else                  fd = (int)syscall(__NR_openat, AT_FDCWD, path, fl, 0666);
    if (fd < 0) return nullptr;
    FILE* fp = fdopen(fd, mode);
    if (!fp) { ::close(fd); return nullptr; }
    return fp;
}

static void sbx_reg_lib(Api* api, dev_t dev, ino_t ino) {
    api->pltHookRegister(dev, ino, "__system_property_get",
                         reinterpret_cast<void*>(sbx_spg),  reinterpret_cast<void**>(&orig_spg));
    api->pltHookRegister(dev, ino, "__system_property_read",
                         reinterpret_cast<void*>(sbx_spr),  reinterpret_cast<void**>(&orig_spr));
    api->pltHookRegister(dev, ino, "__system_property_read_callback",
                         reinterpret_cast<void*>(sbx_sprcb), reinterpret_cast<void**>(&orig_sprcb));
    api->pltHookRegister(dev, ino, "open",
                         reinterpret_cast<void*>(sbx_open),   reinterpret_cast<void**>(&orig_open));
    api->pltHookRegister(dev, ino, "openat",
                         reinterpret_cast<void*>(sbx_openat), reinterpret_cast<void**>(&orig_openat));
    api->pltHookRegister(dev, ino, "fopen",
                         reinterpret_cast<void*>(sbx_fopen),  reinterpret_cast<void**>(&orig_fopen));
    api->pltHookRegister(dev, ino, "open64",
                         reinterpret_cast<void*>(sbx_open),   reinterpret_cast<void**>(&orig_open));
    api->pltHookRegister(dev, ino, "openat64",
                         reinterpret_cast<void*>(sbx_openat), reinterpret_cast<void**>(&orig_openat));
    api->pltHookRegister(dev, ino, "fopen64",
                         reinterpret_cast<void*>(sbx_fopen),  reinterpret_cast<void**>(&orig_fopen));
}

static int sbx_register_across_libs(Api* api) {
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) return 0;
    std::vector<std::pair<dev_t, ino_t>> seen;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* path = strchr(line, '/');
        if (!path) continue;
        size_t pl = strlen(path);
        if (pl && path[pl - 1] == '\n') path[--pl] = '\0';
        if (pl < 3 || strcmp(path + pl - 3, ".so") != 0) continue;
        if (strstr(path, "sandboxid")) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        bool dup = false;
        for (const auto& p : seen)
            if (p.first == st.st_dev && p.second == st.st_ino) { dup = true; break; }
        if (dup) continue;
        seen.push_back(std::make_pair(st.st_dev, st.st_ino));
        sbx_reg_lib(api, st.st_dev, st.st_ino);
    }
    fclose(f);
    return (int)seen.size();
}

static void install_native_read_hooks(Api* api) {

    if (val("SBX_NATIVE_READ") == "0") { LOGD("L9 disabled via kill switch"); return; }

    uint64_t seed = sbxnr::fnv1a(val("FINGERPRINT") + "|" + val("SERIAL") + "|" + val("ANDROID_ID"));
    g_boot_id = sbxnr::uuid_from_seed(seed);

    const std::string& pmac = val("WIFI_MAC");
    g_wifi_mac = sbxnr::is_valid_mac(pmac) ? pmac
                                           : sbxnr::mac_from_seed(seed ^ 0x9E3779B97F4A7C15ULL);

    g_proc_version = sbxnr::synth_proc_version(val("RELEASE"), val("INCREMENTAL"),
                                               val("BOARD_PLATFORM"), val("HOST"), seed);
    g_ram_gb     = sbxnr::pixel_ram_gb(val("MODEL"));
    g_cpu_action = sbxnr::cpu_action_for(val("SOC_MANUFACTURER"), val("SOC_MODEL"), g_cpu_repl);

    int libs = sbx_register_across_libs(api);
    if (libs == 0) {
        LOGW("L9: no mapped .so to hook — native reads not spoofed (kill-switch keeps flag off)");
        return;
    }
    if (!api->pltHookCommit()) {
        // Hard-reset every orig_* pointer that pltHookRegister may have populated but
        // that Zygisk failed to actually commit. Leaving them set to garbage would make
        // our wrappers call random addresses on the next syscall.
        orig_open   = nullptr;
        orig_openat = nullptr;
        orig_fopen  = nullptr;
        orig_spg    = nullptr;
        orig_spr    = nullptr;
        orig_sprcb  = nullptr;
        g_nr_active = false;
        LOGW("L9: pltHookCommit gagal (%d libs registered) — native reads tak dispoof "
             "(orig_* direset, wrapper akan bypass, nilai asli terlihat)", libs);
        return;
    }
    // Sanity check: at least one PLT slot must have been resolved. If every orig_* is
    // null, our wrappers become no-ops that can't call the real implementation. Fail
    // closed rather than crash the target process on first spoofed read.
    if (!orig_open && !orig_openat && !orig_fopen && !orig_spg && !orig_spr && !orig_sprcb) {
        g_nr_active = false;
        LOGW("L9: commit OK but zero PLT slots resolved (%d libs) — fail-closed", libs);
        return;
    }
    g_nr_active = true;
    LOGD("L9 aktif (%d lib): boot_id=%s mac=%s ram=%dGB cpu=%d "
         "[open=%p openat=%p fopen=%p spg=%p spr=%p sprcb=%p]",
         libs, g_boot_id.c_str(), g_wifi_mac.c_str(), g_ram_gb, g_cpu_action,
         reinterpret_cast<void*>(orig_open),   reinterpret_cast<void*>(orig_openat),
         reinterpret_cast<void*>(orig_fopen),  reinterpret_cast<void*>(orig_spg),
         reinterpret_cast<void*>(orig_spr),    reinterpret_cast<void*>(orig_sprcb));
}

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

// -----------------------------------------------------------------------------
// L1: Java-side Build.* static field spoofing (SetStaticObjectField / SetStaticIntField)
//
// References (validated 2026-08):
//   - AOSP frameworks/base core/java/android/os/Build.java (main branch)
//     https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/java/android/os/Build.java
//   - Zygisk API sample (topjohnwu/zygisk-module-sample @ master)
//     https://github.com/topjohnwu/zygisk-module-sample/blob/master/module/jni/zygisk.hpp
//
// Field-type map (from Build.java main):
//   String       : BRAND, MANUFACTURER, MODEL, DEVICE, PRODUCT, BOARD, HARDWARE,
//                  SOC_MANUFACTURER, SOC_MODEL, FINGERPRINT, ID, DISPLAY,
//                  BOOTLOADER, HOST, USER, TYPE, TAGS, RADIO, SERIAL, SKU, ODM_SKU,
//                  CPU_ABI (deprecated), CPU_ABI2 (deprecated),
//                  VERSION.RELEASE, VERSION.CODENAME, VERSION.INCREMENTAL,
//                  VERSION.SECURITY_PATCH, VERSION.BASE_OS,
//                  VERSION.RELEASE_OR_CODENAME (@NonNull), VERSION.RELEASE_OR_PREVIEW_DISPLAY (@NonNull),
//                  VERSION.PREVIEW_SDK_FINGERPRINT (@NonNull, default "REL")
//   String[]     : SUPPORTED_ABIS, SUPPORTED_32_BIT_ABIS, SUPPORTED_64_BIT_ABIS
//   int          : VERSION.SDK_INT, VERSION.PREVIEW_SDK_INT, VERSION.MEDIA_PERFORMANCE_CLASS
//   long         : TIME (= getLong("ro.build.date.utc") * 1000)
// -----------------------------------------------------------------------------

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

static void set_long(JNIEnv* env, jclass c, const char* f, jlong v) {
    jfieldID id = env->GetStaticFieldID(c, f, "J");
    if (!id || env->ExceptionCheck()) { env->ExceptionClear(); return; }
    env->SetStaticLongField(c, id, v);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// Split "a,b,c" -> ["a","b","c"] with whitespace trimming. Empty tokens dropped.
static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    while (i < n) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = n;
        // trim
        size_t a = i;
        while (a < j && (s[a] == ' ' || s[a] == '\t')) ++a;
        size_t b = j;
        while (b > a && (s[b-1] == ' ' || s[b-1] == '\t')) --b;
        if (b > a) out.emplace_back(s.data() + a, b - a);
        i = j + 1;
    }
    return out;
}

// Set a static String[] field. Silently no-ops if `v` is empty (preserves original value).
static void set_str_array(JNIEnv* env, jclass c, const char* f, const std::string& v) {
    if (v.empty()) return;
    std::vector<std::string> parts = split_csv(v);
    if (parts.empty()) return;

    jfieldID id = env->GetStaticFieldID(c, f, "[Ljava/lang/String;");
    if (!id || env->ExceptionCheck()) { env->ExceptionClear(); return; }

    jclass str_cls = env->FindClass("java/lang/String");
    if (!str_cls || env->ExceptionCheck()) { env->ExceptionClear(); return; }

    jobjectArray arr = env->NewObjectArray(static_cast<jsize>(parts.size()), str_cls, nullptr);
    if (!arr || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(str_cls);
        return;
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        jstring j = env->NewStringUTF(parts[i].c_str());
        if (!j || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        env->SetObjectArrayElement(arr, static_cast<jsize>(i), j);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(j);
    }
    env->SetStaticObjectField(c, id, arr);
    if (env->ExceptionCheck()) env->ExceptionClear();

    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(str_cls);
}

static void install_build_hook(JNIEnv* env) {
    jclass build = env->FindClass("android/os/Build");
    if (build && !env->ExceptionCheck()) {

        // Plain String fields
        // NOTE: SERIAL is initialized in AOSP from getString("no.such.thing") — literally "unknown"
        // by default. AppLog SDK still reads Build.SERIAL and hashes it, so we spoof it if
        // the identity blob provides one. SKU / ODM_SKU came in Android 12 (API 31) and read
        // ro.boot.hardware.sku / ro.boot.product.hardware.sku respectively. Adding them
        // is safe on older releases: GetStaticFieldID returns null → set_str no-ops.
        static const std::pair<const char*, const char*> f[] = {
            {"BRAND","BRAND"}, {"MANUFACTURER","MANUFACTURER"},
            {"MODEL","MODEL"}, {"DEVICE","DEVICE"}, {"PRODUCT","PRODUCT"},
            {"BOARD","BOARD"}, {"HARDWARE","HARDWARE"},
            {"SOC_MANUFACTURER","SOC_MANUFACTURER"}, {"SOC_MODEL","SOC_MODEL"},
            {"FINGERPRINT","FINGERPRINT"}, {"ID","ID"},
            {"DISPLAY","DISPLAY"}, {"BOOTLOADER","BOOTLOADER"},
            {"HOST","HOST"}, {"USER","USER"}, {"TYPE","TYPE"},
            {"TAGS","TAGS"}, {"RADIO","RADIO"},
            // Added Phase 3 (2026-08):
            {"SERIAL","SERIAL"},
            {"SKU","SKU"},                   // Android 12+ (API 31)
            {"ODM_SKU","ODM_SKU"},           // Android 12+ (API 31)
            {"CPU_ABI","CPU_ABI"},           // Deprecated (API 21) but still read by SDKs
            {"CPU_ABI2","CPU_ABI2"},         // Deprecated (API 21) but still read by SDKs
        };
        for (const auto& [fn, k] : f) set_str(env, build, fn, val(k));

        // Build.TIME (long) = getLong("ro.build.date.utc") * 1000
        // (AOSP Build.java main). Spoof from identity so the build date
        // matches the persona fingerprint instead of the real device.
        const std::string& butc = val("BUILD_TIME_UTC");
        if (!butc.empty()) {
            long long t = 0;
            if (sbx_parse_ll(butc, t) && t > 0)
                set_long(env, build, "TIME", (jlong)t * 1000);
        }

        // String[] array fields — SUPPORTED_ABIS family. AppLog & AntiCheat SDKs often
        // read these to fingerprint 32/64-bit capability. Values are CSV in identity blob:
        //   SUPPORTED_ABIS       = "arm64-v8a,armeabi-v7a,armeabi"
        //   SUPPORTED_64_BIT_ABIS= "arm64-v8a"
        //   SUPPORTED_32_BIT_ABIS= "armeabi-v7a,armeabi"
        set_str_array(env, build, "SUPPORTED_ABIS",        val("SUPPORTED_ABIS"));
        set_str_array(env, build, "SUPPORTED_32_BIT_ABIS", val("SUPPORTED_32_BIT_ABIS"));
        set_str_array(env, build, "SUPPORTED_64_BIT_ABIS", val("SUPPORTED_64_BIT_ABIS"));

        env->DeleteLocalRef(build);
    } else env->ExceptionClear();

    jclass ver = env->FindClass("android/os/Build$VERSION");
    if (ver && !env->ExceptionCheck()) {
        set_str(env, ver, "RELEASE",        val("RELEASE"));

        set_str(env, ver, "CODENAME",       std::string("REL"));
        set_str(env, ver, "INCREMENTAL",    val("INCREMENTAL"));
        set_str(env, ver, "SECURITY_PATCH", val("SECURITY_PATCH"));

        // Added Phase 3: BASE_OS (String, API 23+) reads ro.build.version.base_os.
        // Empty by default on stock builds; but some fingerprint SDKs sniff it for OTAs.
        set_str(env, ver, "BASE_OS",        val("BASE_OS"));

        // Non-null String fields (API 30+/31+): defensively spoof to plain RELEASE.
        // GetStaticFieldID silently no-ops on older APIs.
        const std::string& rel = val("RELEASE");
        if (!rel.empty()) {
            set_str(env, ver, "RELEASE_OR_CODENAME",        rel);  // API 30
            set_str(env, ver, "RELEASE_OR_PREVIEW_DISPLAY", rel);  // API 31
        }

        // PREVIEW_SDK_FINGERPRINT: on production it's the literal "REL". Force it,
        // because Google internal & canary builds leak build-time hashes here.
        set_str(env, ver, "PREVIEW_SDK_FINGERPRINT", std::string("REL"));

        const std::string& s = val("SDK_INT");
        if (!s.empty()) {
            int sdk = std::atoi(s.c_str());
            if (sdk > 0) set_int(env, ver, "SDK_INT", sdk);
        }

        // Added Phase 3: PREVIEW_SDK_INT (int, API 23+) — should be 0 on production
        // builds. Setting to 0 explicitly matches all Pixel personas we ship.
        set_int(env, ver, "PREVIEW_SDK_INT", 0);

        // Added Phase 3: MEDIA_PERFORMANCE_CLASS (int, API 31+). Only spoof if
        // identity provides an explicit value (Pixel 6+ = 31, Pixel 8 = 33, etc).
        const std::string& mpc = val("MEDIA_PERFORMANCE_CLASS");
        if (!mpc.empty()) {
            int v = std::atoi(mpc.c_str());
            if (v >= 0) set_int(env, ver, "MEDIA_PERFORMANCE_CLASS", v);
        }

        env->DeleteLocalRef(ver);
    } else env->ExceptionClear();
}

static void request_companion_mounts(int fd) {
    uint8_t cmd  = sandboxid::CMD_DO_MOUNTS;
    uint32_t pid = (uint32_t)::getpid();
    if (!sandboxid::write_full(fd, &cmd, 1) || !sandboxid::write_full(fd, &pid, sizeof(pid))) {

        LOGW("companion DO_MOUNTS write failed (socket unusable post-specialize?)");
        return;
    }
    uint32_t ok = 0;
    if (!sandboxid::read_full(fd, &ok, sizeof(ok))) { LOGW("companion mount ack failed"); return; }
    LOGI("bind-mount via companion: %u ok (pid=%u)", ok, pid);
}

static void request_companion_hide(int fd) {
    uint8_t cmd  = sandboxid::CMD_DO_HIDE;
    uint32_t pid = (uint32_t)::getpid();
    if (!sandboxid::write_full(fd, &cmd, 1) || !sandboxid::write_full(fd, &pid, sizeof(pid))) {
        LOGW("companion DO_HIDE write failed (socket unusable post-specialize?)");
        return;
    }
    uint32_t n = 0;
    if (!sandboxid::read_full(fd, &n, sizeof(n))) { LOGW("companion hide ack failed"); return; }
    LOGI("root/mount-trace hide via companion: %u detached (pid=%u)", n, pid);
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
        LOGD("parse_blob: %zu identity keys", g_id.size());

        install_build_hook(env_);
        install_prop_hook(api_, env_);
        install_leak_sensors(api_, env_);
        install_uptime_hook(api_, env_);
        install_native_read_hooks(api_);
#ifdef SBX_DEBUG
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
            while (!v.empty() && (v.back()=='\r' || v.back()=='\n' || v.back()==' '))
                v.pop_back();
            if (!k.empty()) g_id[k] = v;
        }
    }
};

REGISTER_ZYGISK_MODULE(SandboxID)

extern "C" void sandboxid_companion(int client);
REGISTER_ZYGISK_COMPANION(sandboxid_companion)
