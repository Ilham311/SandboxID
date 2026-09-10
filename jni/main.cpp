
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
#include <dlfcn.h>
#include <android/log.h>
#include <string>
#include <map>
#include <mutex>
#include <vector>
#include <utility>
#include <ctime>
#include "zygisk.hpp"
#include "config.hpp"
#include "sbx_identity.hpp"
#include "sbx_property.hpp"
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
    return it != g_id.end() ? it->second : empty;
}

static jstring (*orig_native_get)(JNIEnv*, jclass, jstring, jstring) = nullptr;
static jlong (*orig_native_find)(JNIEnv*, jclass, jstring) = nullptr;
static jstring (*orig_handle_get)(JNIEnv*, jclass, jlong) = nullptr;
static jint (*orig_handle_get_int)(JNIEnv*, jclass, jlong, jint) = nullptr;
static jlong (*orig_handle_get_long)(JNIEnv*, jclass, jlong, jlong) = nullptr;
static jboolean (*orig_handle_get_bool)(JNIEnv*, jclass, jlong, jboolean) = nullptr;
static sbxprop::HandleNames g_prop_handles;

static bool string_from_jni(JNIEnv* env, jstring value, std::string& out) {
    if (!value) return false;
    const char* raw = env->GetStringUTFChars(value, nullptr);
    if (!raw || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }
    out.assign(raw);
    env->ReleaseStringUTFChars(value, raw);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        out.clear();
        return false;
    }
    return true;
}

static bool property_name_for_handle(jlong handle, std::string& name) {
    return g_prop_handles.find(static_cast<int64_t>(handle), name);
}

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

        {"ro.product.system.marketname",    "MARKETNAME"},
        {"ro.product.odm.marketname",       "MARKETNAME"},
        {"ro.product.product.marketname",   "MARKETNAME"},
        {"ro.build.id",                     "ID"},
        {"ro.build.display.id",             "DISPLAY"},
        {"ro.build.description",            "DESCRIPTION"},
        {"ro.build.version.release",        "RELEASE"},
        {"ro.build.version.security_patch", "SECURITY_PATCH"},
        {"ro.build.version.incremental",    "INCREMENTAL"},
        {"gsm.version.baseband",            "RADIO"},
        {"ro.build.user",                   "USER"},
        {"ro.build.host",                   "HOST"},
        {"ro.build.tags",                   "TAGS"},
        {"ro.build.type",                   "TYPE"},

        {"ro.build.date.utc",               "BUILD_TIME_UTC"},
        {"ro.build.date",                   "BUILD_DATE"},

        {"ro.build.flavor",                 "FLAVOR"},
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

        {"ro.boot.hardware.sku",                     "SKU"},
        {"ro.boot.product.hardware.sku",             "ODM_SKU"},

        {"ro.product.build.id",                  "ID"},
        {"ro.system.build.id",                   "ID"},
        {"ro.system_ext.build.id",               "ID"},
        {"ro.vendor.build.id",                   "ID"},
        {"ro.odm.build.id",                      "ID"},

        {"ro.product.build.version.incremental",          "INCREMENTAL"},
        {"ro.system.build.version.incremental",           "INCREMENTAL"},
        {"ro.system_ext.build.version.incremental",       "INCREMENTAL"},
        {"ro.vendor.build.version.incremental",           "INCREMENTAL"},
        {"ro.odm.build.version.incremental",              "INCREMENTAL"},

        {"ro.product.build.version.release",              "RELEASE"},
        {"ro.system.build.version.release",               "RELEASE"},
        {"ro.system_ext.build.version.release",           "RELEASE"},
        {"ro.vendor.build.version.release",               "RELEASE"},
        {"ro.odm.build.version.release",                  "RELEASE"},

        {"ro.product.build.version.release_or_codename",  "RELEASE"},
        {"ro.system.build.version.release_or_codename",   "RELEASE"},
        {"ro.system_ext.build.version.release_or_codename", "RELEASE"},
        {"ro.vendor.build.version.release_or_codename",   "RELEASE"},
        {"ro.odm.build.version.release_or_codename",      "RELEASE"},

        {"ro.product.build.date.utc",                     "BUILD_TIME_UTC"},
        {"ro.system.build.date.utc",                      "BUILD_TIME_UTC"},
        {"ro.system_ext.build.date.utc",                  "BUILD_TIME_UTC"},
        {"ro.vendor.build.date.utc",                      "BUILD_TIME_UTC"},
        {"ro.odm.build.date.utc",                         "BUILD_TIME_UTC"},
        {"ro.bootimage.build.date.utc",                   "BUILD_TIME_UTC"},

        {"ro.product.build.date",                         "BUILD_DATE"},
        {"ro.system.build.date",                          "BUILD_DATE"},
        {"ro.system_ext.build.date",                      "BUILD_DATE"},
        {"ro.vendor.build.date",                          "BUILD_DATE"},
        {"ro.odm.build.date",                             "BUILD_DATE"},
        {"ro.bootimage.build.date",                       "BUILD_DATE"},

        {"ro.product.build.type",                         "TYPE"},
        {"ro.system.build.type",                          "TYPE"},
        {"ro.system_ext.build.type",                      "TYPE"},
        {"ro.vendor.build.type",                          "TYPE"},
        {"ro.odm.build.type",                             "TYPE"},

        {"ro.product.build.tags",                         "TAGS"},
        {"ro.system.build.tags",                          "TAGS"},
        {"ro.system_ext.build.tags",                      "TAGS"},
        {"ro.vendor.build.tags",                          "TAGS"},
        {"ro.odm.build.tags",                             "TAGS"},
    };
    return m;
}

static bool g_stable_release_runtime = false;

static bool spoof_prop_value(const std::string& k, std::string& out) {
    if (sbxprop::release_alias_property(k) && !g_stable_release_runtime)
        return false;
    const auto& map = prop_to_identity_map();
    auto it = map.find(k);
    if (it != map.end()) {
        const std::string& v = val(it->second);
        if (!v.empty()) { out = v; return true; }
    }
    return false;
}

static inline bool sbx_prop_hidden(const char* name) {
    return name && sbxnr::should_hide_prop(name);
}

static jstring checked_new_string(JNIEnv* env, const std::string& value, jstring fallback) {
    jstring result = env->NewStringUTF(value.c_str());
    if (!result || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return fallback;
    }
    return result;
}

static jstring hook_prop_get(JNIEnv* env, jclass clazz, jstring j_key, jstring j_def) {
    if (!j_key) return j_def;

    std::string k;
    if (!string_from_jni(env, j_key, k)) return j_def;
    LOGD("L2 native_get('%s')", k.c_str());

    std::string v;
    if (spoof_prop_value(k, v)) {
        LOGD("L2 SPOOF '%s' -> '%s'", k.c_str(), v.c_str());
        return checked_new_string(env, v, j_def);
    }

    if (sbx_prop_hidden(k.c_str())) {
        LOGD("L2 HIDE '%s' (report absent)", k.c_str());
        return j_def;
    }

    if (orig_native_get) return orig_native_get(env, clazz, j_key, j_def);

    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(k.c_str(), buf) > 0)
        return checked_new_string(env, buf, j_def);
    return j_def;
}

static jlong hook_prop_find(JNIEnv* env, jclass clazz, jstring j_name) {
    if (!orig_native_find) return 0;
    jlong handle = orig_native_find(env, clazz, j_name);
    if (env->ExceptionCheck()) return handle;
    if (handle == 0 || !j_name) return handle;

    std::string name;
    if (!string_from_jni(env, j_name, name)) return handle;
    g_prop_handles.remember(static_cast<int64_t>(handle), name);
    return handle;
}

static jstring hook_prop_handle_get(JNIEnv* env, jclass clazz, jlong handle) {
    std::string name;
    if (property_name_for_handle(handle, name)) {
        std::string value;
        if (spoof_prop_value(name, value))
            return checked_new_string(env, value, nullptr);
        if (sbx_prop_hidden(name.c_str())) {
            LOGD("L2 Handle HIDE '%s'", name.c_str());
            return checked_new_string(env, "", nullptr);
        }
    }
    return orig_handle_get ? orig_handle_get(env, clazz, handle) : nullptr;
}

static bool parse_prop_int64(const std::string& value, long long min_value,
                             long long max_value, long long& out) {
    int64_t parsed = 0;
    if (!sbxprop::parse_int64(value, min_value, max_value, parsed)) return false;
    out = static_cast<long long>(parsed);
    return true;
}

static bool parse_prop_bool(const std::string& value, jboolean& out) {
    bool parsed = false;
    if (!sbxprop::parse_bool(value, parsed)) return false;
    out = parsed ? JNI_TRUE : JNI_FALSE;
    return true;
}

static jint hook_prop_handle_get_int(JNIEnv* env, jclass clazz, jlong handle,
                                     jint def) {
    std::string name;
    if (property_name_for_handle(handle, name)) {
        std::string value;
        if (spoof_prop_value(name, value)) {
            long long parsed = 0;
            return parse_prop_int64(value, INT32_MIN, INT32_MAX, parsed)
                ? static_cast<jint>(parsed) : def;
        }
        if (sbx_prop_hidden(name.c_str())) return def;
    }
    return orig_handle_get_int ? orig_handle_get_int(env, clazz, handle, def) : def;
}

static jlong hook_prop_handle_get_long(JNIEnv* env, jclass clazz, jlong handle,
                                       jlong def) {
    std::string name;
    if (property_name_for_handle(handle, name)) {
        std::string value;
        if (spoof_prop_value(name, value)) {
            long long parsed = 0;
            return parse_prop_int64(value, INT64_MIN, INT64_MAX, parsed)
                ? static_cast<jlong>(parsed) : def;
        }
        if (sbx_prop_hidden(name.c_str())) return def;
    }
    return orig_handle_get_long ? orig_handle_get_long(env, clazz, handle, def) : def;
}

static jboolean hook_prop_handle_get_bool(JNIEnv* env, jclass clazz, jlong handle,
                                          jboolean def) {
    std::string name;
    if (property_name_for_handle(handle, name)) {
        std::string value;
        if (spoof_prop_value(name, value)) {
            jboolean parsed = def;
            return parse_prop_bool(value, parsed) ? parsed : def;
        }
        if (sbx_prop_hidden(name.c_str())) return def;
    }
    return orig_handle_get_bool ? orig_handle_get_bool(env, clazz, handle, def) : def;
}

static void install_prop_handle_hooks(Api* api, JNIEnv* env) {
    JNINativeMethod methods[] = {
        {const_cast<char*>("native_find"),
         const_cast<char*>("(Ljava/lang/String;)J"),
         reinterpret_cast<void*>(hook_prop_find)},
        {const_cast<char*>("native_get"),
         const_cast<char*>("(J)Ljava/lang/String;"),
         reinterpret_cast<void*>(hook_prop_handle_get)},
        {const_cast<char*>("native_get_int"),
         const_cast<char*>("(JI)I"),
         reinterpret_cast<void*>(hook_prop_handle_get_int)},
        {const_cast<char*>("native_get_long"),
         const_cast<char*>("(JJ)J"),
         reinterpret_cast<void*>(hook_prop_handle_get_long)},
        {const_cast<char*>("native_get_boolean"),
         const_cast<char*>("(JZ)Z"),
         reinterpret_cast<void*>(hook_prop_handle_get_bool)},
    };
    api->hookJniNativeMethods(env, "android/os/SystemProperties", methods,
                              sizeof(methods) / sizeof(methods[0]));
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (methods[0].fnPtr)
        orig_native_find = reinterpret_cast<jlong (*)(JNIEnv*, jclass, jstring)>(
            methods[0].fnPtr);
    if (methods[1].fnPtr)
        orig_handle_get = reinterpret_cast<jstring (*)(JNIEnv*, jclass, jlong)>(
            methods[1].fnPtr);
    if (methods[2].fnPtr)
        orig_handle_get_int = reinterpret_cast<jint (*)(JNIEnv*, jclass, jlong, jint)>(
            methods[2].fnPtr);
    if (methods[3].fnPtr)
        orig_handle_get_long = reinterpret_cast<jlong (*)(JNIEnv*, jclass, jlong, jlong)>(
            methods[3].fnPtr);
    if (methods[4].fnPtr)
        orig_handle_get_bool = reinterpret_cast<jboolean (*)(JNIEnv*, jclass, jlong, jboolean)>(
            methods[4].fnPtr);
    if (!orig_native_find || !orig_handle_get || !orig_handle_get_int ||
        !orig_handle_get_long || !orig_handle_get_bool) {
        LOGW("L2: SystemProperties Handle API unavailable/partial (%d/%d/%d/%d/%d)",
             orig_native_find ? 1 : 0, orig_handle_get ? 1 : 0,
             orig_handle_get_int ? 1 : 0, orig_handle_get_long ? 1 : 0,
             orig_handle_get_bool ? 1 : 0);
    } else {
        LOGD("L2 SystemProperties Handle hooks installed");
    }
}

static void install_prop_hook(Api* api, JNIEnv* env) {
    JNINativeMethod m = {
        const_cast<char*>("native_get"),
        const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
        reinterpret_cast<void*>(hook_prop_get),
    };

    api->hookJniNativeMethods(env, "android/os/SystemProperties", &m, 1);
    if (env->ExceptionCheck())
        env->ExceptionClear();
    orig_native_get = reinterpret_cast<jstring (*)(JNIEnv*, jclass, jstring, jstring)>(m.fnPtr);
    if (!orig_native_get)
        LOGE("L2: native_get hook FAILED (fnPtr null) — prop spoofing off for this process");
    else
        LOGD("L2 native_get hooked (orig=%p)", reinterpret_cast<void*>(orig_native_get));
}

static jint     (*orig_get_int)(JNIEnv*, jclass, jstring, jint)     = nullptr;
static jlong    (*orig_get_long)(JNIEnv*, jclass, jstring, jlong)   = nullptr;
static jboolean (*orig_get_bool)(JNIEnv*, jclass, jstring, jboolean)= nullptr;

static const std::map<std::string, jboolean>& sbx_bool_spoof() {
    static const std::map<std::string, jboolean> m;
    return m;
}
static const std::map<std::string, jint>& sbx_int_spoof() {
    static const std::map<std::string, jint> m;
    return m;
}
static const std::map<std::string, jlong>& sbx_long_spoof() {
    static const std::map<std::string, jlong> m;
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

static bool sbx_parse_ll(const std::string& v, long long& out) {
    return parse_prop_int64(v, INT64_MIN, INT64_MAX, out);
}

static int g_build_replacements = 0;
static int g_build_failures = 0;

static void clear_jni_exception(JNIEnv* env) {
    if (env->ExceptionCheck()) env->ExceptionClear();
}

static void set_str(JNIEnv* env, jclass c, const char* f,
                    const std::string& v) {
    if (v.empty()) return;
    jfieldID id = env->GetStaticFieldID(c, f, "Ljava/lang/String;");
    if (!id || env->ExceptionCheck()) {
        clear_jni_exception(env);
        ++g_build_failures;
        return;
    }
    jstring j = env->NewStringUTF(v.c_str());
    if (!j || env->ExceptionCheck()) {
        clear_jni_exception(env);
        ++g_build_failures;
        return;
    }
    env->SetStaticObjectField(c, id, j);
    if (env->ExceptionCheck()) {
        clear_jni_exception(env);
        ++g_build_failures;
    } else {
        ++g_build_replacements;
    }
    env->DeleteLocalRef(j);
}

static void set_long(JNIEnv* env, jclass c, const char* f, jlong v) {
    jfieldID id = env->GetStaticFieldID(c, f, "J");
    if (!id || env->ExceptionCheck()) {
        clear_jni_exception(env);
        ++g_build_failures;
        return;
    }
    env->SetStaticLongField(c, id, v);
    if (env->ExceptionCheck()) {
        clear_jni_exception(env);
        ++g_build_failures;
    } else {
        ++g_build_replacements;
    }
}

static jint hook_prop_get_int(JNIEnv* env, jclass clazz, jstring j_key, jint def) {
    if (!j_key) return def;
    std::string k;
    if (!string_from_jni(env, j_key, k)) return def;

    std::string v;
    if (spoof_prop_value(k, v)) {
        long long n = 0;
        if (!parse_prop_int64(v, INT32_MIN, INT32_MAX, n)) return def;
        LOGD("L7 SPI(id) '%s' -> %d", k.c_str(), (int)n);
        return static_cast<jint>(n);
    }

    const auto& m = sbx_int_spoof();
    auto it = m.find(k);
    if (it != m.end()) { LOGD("L7 SPI '%s' -> %d", k.c_str(), it->second); return it->second; }
    if (sbx_prop_hidden(k.c_str()) || sbx_should_suppress_key(k)) {
        LOGD("L7 SPI SUPPRESS '%s'", k.c_str());
        return def;
    }
    return orig_get_int ? orig_get_int(env, clazz, j_key, def) : def;
}
static jlong hook_prop_get_long(JNIEnv* env, jclass clazz, jstring j_key, jlong def) {
    if (!j_key) return def;
    std::string k;
    if (!string_from_jni(env, j_key, k)) return def;

    std::string v;
    if (spoof_prop_value(k, v)) {
        long long n = 0;
        if (!parse_prop_int64(v, INT64_MIN, INT64_MAX, n)) return def;
        LOGD("L7 SPL(id) '%s' -> %lld", k.c_str(), (long long)n);
        return static_cast<jlong>(n);
    }

    const auto& m = sbx_long_spoof();
    auto it = m.find(k);
    if (it != m.end()) { LOGD("L7 SPL '%s' -> %lld", k.c_str(), (long long)it->second); return it->second; }
    if (sbx_prop_hidden(k.c_str()) || sbx_should_suppress_key(k)) {
        LOGD("L7 SPL SUPPRESS '%s'", k.c_str());
        return def;
    }
    return orig_get_long ? orig_get_long(env, clazz, j_key, def) : def;
}
static jboolean hook_prop_get_bool(JNIEnv* env, jclass clazz, jstring j_key, jboolean def) {
    if (!j_key) return def;
    std::string k;
    if (!string_from_jni(env, j_key, k)) return def;
    std::string v;
    if (spoof_prop_value(k, v)) {
        jboolean parsed = def;
        return parse_prop_bool(v, parsed) ? parsed : def;
    }
    const auto& m = sbx_bool_spoof();
    auto it = m.find(k);
    if (it != m.end()) { LOGD("L7 SPB '%s' -> %d", k.c_str(), (int)it->second); return it->second; }
    if (sbx_prop_hidden(k.c_str()) || sbx_should_suppress_key(k)) {
        LOGD("L7 SPB SUPPRESS '%s'", k.c_str());
        return def;
    }
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
    if (env->ExceptionCheck())
        env->ExceptionClear();
    if (m[0].fnPtr)
        orig_get_int = reinterpret_cast<jint (*)(JNIEnv*, jclass, jstring, jint)>(m[0].fnPtr);
    if (m[1].fnPtr)
        orig_get_long = reinterpret_cast<jlong (*)(JNIEnv*, jclass, jstring, jlong)>(m[1].fnPtr);
    if (m[2].fnPtr)
        orig_get_bool = reinterpret_cast<jboolean (*)(JNIEnv*, jclass, jstring, jboolean)>(m[2].fnPtr);
    if (!m[0].fnPtr || !m[1].fnPtr || !m[2].fnPtr)
        LOGE("L7: leak-sensor hooks FAILED (fnPtr null: %d/%d/%d) — typed getters unspoofed",
             m[0].fnPtr ? 1 : 0, m[1].fnPtr ? 1 : 0, m[2].fnPtr ? 1 : 0);
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

    static const char* const kLibs[] = {
        "/libutils.so",
        "/libandroid_runtime.so",
    };
    int registered = 0;
    if (!orig_clock_gettime)
        orig_clock_gettime = reinterpret_cast<int (*)(clockid_t, timespec*)>(
            dlsym(RTLD_DEFAULT, "clock_gettime"));
    for (size_t i = 0; i < sizeof(kLibs) / sizeof(kLibs[0]); ++i) {
        dev_t dev = 0; ino_t ino = 0;
        if (!sbx_lib_dev_inode(kLibs[i], &dev, &ino)) continue;
        api->pltHookRegister(dev, ino, "clock_gettime",
                             reinterpret_cast<void*>(sbx_hooked_clock_gettime),
                             reinterpret_cast<void**>(&orig_clock_gettime));
        ++registered;
    }
    if (registered == 0) {
        g_boot_off_sec = 0;
        LOGW("L8: chokepoint lib tak ketemu (scanned %zu) — uptime tak dispoof",
             sizeof(kLibs) / sizeof(kLibs[0]));
        return;
    }
    if (!api->pltHookCommit()) {

        g_boot_off_sec = 0;
        LOGW("L8: pltHookCommit gagal (%d/%zu libs registered) — offset dinolkan, "
             "semua reader lihat jam asli (divergensi hang dihindari)",
             registered, sizeof(kLibs) / sizeof(kLibs[0]));
        return;
    }
    if (orig_clock_gettime == nullptr) {

        g_boot_off_sec = 0;
        return;
    }
    LOGD("L8 boottime PLT hook aktif (+%llds, %d/%zu lib mapped) orig=%p",
         (long long)secs, registered, sizeof(kLibs) / sizeof(kLibs[0]),
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
static std::string g_proc_version;
static int         g_ram_gb = 0;
static sbxnr::EnvironmentGates g_environment_gates;

static std::string    g_pkg;
static sbxnr::ApplogIds g_applog;
static bool           g_applog_ok = false;

static void sbx_fill_prop(char* value, const std::string& v) {
    size_t n = v.size();
    if (n > PROP_VALUE_MAX - 1) n = PROP_VALUE_MAX - 1;
    memcpy(value, v.data(), n);
    value[n] = '\0';
}

static inline bool sbx_nr_spoofable(const char* name) {
    return g_nr_active && name && !sbxnr::is_native_unsafe_prop(name);
}

static sbxprop::ValueDecision sbx_prop_decision(const char* name) {
    if (!sbx_nr_spoofable(name)) return {};
    if (sbx_prop_hidden(name))
        return sbxprop::decide_value(true, true, nullptr);
    std::string mapped;
    if (spoof_prop_value(name, mapped))
        return sbxprop::decide_value(true, false, &mapped);
    return {};
}

static int sbx_spg(const char* name, char* value) {
    if (value) {
        const sbxprop::ValueDecision decision = sbx_prop_decision(name);
        if (decision.action == sbxprop::ValueAction::kHidden) {
            value[0] = '\0';
            return 0;
        }
        if (decision.action == sbxprop::ValueAction::kMapped) {
            sbx_fill_prop(value, decision.mapped);
            return static_cast<int>(sbxprop::legacy_copy_length(decision.mapped));
        }
    }
    if (orig_spg) return orig_spg(name, value);
    if (value) value[0] = '\0';
    return 0;
}

static int sbx_spr(const void* pi, char* name, char* value) {
    int r = orig_spr ? orig_spr(pi, name, value) : -1;
    if (r >= 0 && value) {
        const sbxprop::ValueDecision decision = sbx_prop_decision(name);
        if (decision.action == sbxprop::ValueAction::kHidden) {
            value[0] = '\0';
            return 0;
        }
        if (decision.action == sbxprop::ValueAction::kMapped) {
            sbx_fill_prop(value, decision.mapped);
            return static_cast<int>(sbxprop::legacy_copy_length(decision.mapped));
        }
    }
    return r;
}

struct SbxCbCtx {
    sbxprop::CallbackRelay<sbx_prop_cb> relay;
};
static void sbx_cb_tramp(void* cookie, const char* name, const char* value,
                         uint32_t serial) {
    SbxCbCtx* c = static_cast<SbxCbCtx*>(cookie);
    if (!c) return;
    c->relay.complete(name, value, serial, sbx_prop_decision(name));
}
static void sbx_sprcb(const void* pi, sbx_prop_cb cb, void* cookie) {
    SbxCbCtx ctx{{cb, cookie, false}};
    (void)sbxprop::dispatch_callback_read(
        orig_sprcb, pi, cb, sbx_cb_tramp, &ctx);
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

static bool sbx_build_content(sbxnr::Kind kind, const char* path, std::string& out) {
    if (!sbxnr::environment_surface_enabled(kind, g_environment_gates))
        return false;
    switch (kind) {
        case sbxnr::BOOTID:  out = g_boot_id; out.push_back('\n'); return true;
        case sbxnr::VERSION:
            out = g_proc_version; out.push_back('\n'); return true;
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
            std::string real = sbx_read_real("/proc/cpuinfo");
            if (real.empty()) return false;
            return sbxnr::patch_cpuinfo_aggregate_revision(real, out);
        }
        case sbxnr::APPLOG_XML: {
            if (!g_applog_ok) return false;
            std::string real = sbx_read_real(path);
            if (real.empty()) { out = sbxnr::applog_xml_synth(g_applog); return true; }
            return sbxnr::patch_applog_xml(real, g_applog, out);
        }
        case sbxnr::BD_RAW_DID:        if (g_applog_ok) { out = g_applog.did;        out.push_back('\n'); return true; } return false;
        case sbxnr::BD_RAW_IID:        if (g_applog_ok) { out = g_applog.iid;        out.push_back('\n'); return true; } return false;
        case sbxnr::BD_RAW_OPENUDID:   if (g_applog_ok) { out = g_applog.openudid;   out.push_back('\n'); return true; } return false;
        case sbxnr::BD_RAW_CLIENTUDID: if (g_applog_ok) { out = g_applog.clientudid; out.push_back('\n'); return true; } return false;
        case sbxnr::BD_RAW_CDID:       if (g_applog_ok) { out = g_applog.cdid;       out.push_back('\n'); return true; } return false;
        default: return false;
    }
}

static int sbx_spoof_fd(const char* path) {
    sbxnr::Kind kind = sbxnr::classify(path);
    if (kind == sbxnr::NONE) return -1;
    std::string content;
    if (!sbx_build_content(kind, path, content)) return -1;
    int fd = sbx_make_memfd(content);
    if (fd >= 0) LOGD("L9 redirect '%s' -> memfd (%zu B)", path, content.size());
    return fd;
}

static inline bool sbx_is_pure_read(int flags) {
    return (flags & O_ACCMODE) == O_RDONLY && !(flags & (O_CREAT | O_TMPFILE));
}

static bool sbx_resolve_openat_path(int dirfd, const char* pathname,
                                    std::string& resolved) {
    if (!pathname || !*pathname) return false;
    std::string path(pathname);
    if (path[0] == '/')
        return sbxnr::normalize_absolute_path(path, resolved);

    char base[4096];
    if (dirfd == AT_FDCWD) {
        if (!::getcwd(base, sizeof(base))) return false;
    } else {
        char proc_path[64];
        int n = ::snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", dirfd);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(proc_path)) return false;
        ssize_t got = ::readlink(proc_path, base, sizeof(base) - 1);
        if (got <= 0 || static_cast<size_t>(got) >= sizeof(base) - 1) return false;
        base[got] = '\0';
        static const char deleted[] = " (deleted)";
        size_t len = static_cast<size_t>(got);
        if (len >= sizeof(deleted) - 1 &&
            std::memcmp(base + len - (sizeof(deleted) - 1), deleted,
                        sizeof(deleted) - 1) == 0)
            return false;
    }
    return sbxnr::join_and_normalize_path(base, path, resolved);
}

static int sbx_openat(int dirfd, const char* pathname, int flags, ...) {
    mode_t mode = 0;
    bool has_mode = (flags & (O_CREAT | O_TMPFILE)) != 0;
    if (has_mode) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }

    if (g_nr_active && pathname && sbx_is_pure_read(flags)) {
        std::string resolved;
        if (sbx_resolve_openat_path(dirfd, pathname, resolved)) {
            int fd = sbx_spoof_fd(resolved.c_str());
            if (fd >= 0) return fd;
        }
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

static int sbx_fopen_flags(const char* mode) {
    bool plus = mode && std::strchr(mode, '+');
    switch (mode ? mode[0] : 'r') {
        case 'w': return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
        case 'a': return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
        default:  return plus ? O_RDWR : O_RDONLY;
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
        const char* basename = strrchr(path, '/');
        basename = basename ? basename + 1 : path;
        LOGD("L9 PLT register %s dev=%llu ino=%llu", basename,
             static_cast<unsigned long long>(st.st_dev),
             static_cast<unsigned long long>(st.st_ino));
        sbx_reg_lib(api, st.st_dev, st.st_ino);
    }
    fclose(f);
    LOGD("L9 PLT scan registered=%zu; libraries loaded after this scan are "
         "outside confirmed coverage", seen.size());
    return static_cast<int>(seen.size());
}

static void install_native_read_hooks(Api* api) {

    g_environment_gates = {};
    if (val("SBX_NATIVE_READ") == "0") { LOGD("L9 disabled via kill switch"); return; }

    uint64_t seed = sbxnr::fnv1a(val("FINGERPRINT") + "|" + val("SERIAL") + "|" + val("ANDROID_ID"));
    g_boot_id = sbxnr::uuid_from_seed(seed);

    g_environment_gates.native_read = true;
    g_environment_gates.proc_version = val("SBX_PROC_VERSION") == "1";
    g_environment_gates.meminfo = val("SBX_MEMINFO") == "1";
    g_environment_gates.cpu_revision = val("SBX_CPU_REVISION") == "1";

    if (g_environment_gates.proc_version) {
        g_proc_version = sbxnr::synth_proc_version(
            val("RELEASE"), val("INCREMENTAL"), val("BOARD_PLATFORM"),
            val("HOST"), seed);
    } else {
        g_proc_version.clear();
    }
    g_ram_gb = g_environment_gates.meminfo ? sbxnr::pixel_ram_gb(val("MODEL")) : 0;

    if (!g_pkg.empty()) {
        uint64_t epoch_ms = strtoull(val("APPLOG_EPOCH").c_str(), nullptr, 10);
        if (epoch_ms <= 0) epoch_ms = 1700000000000ULL;
        uint64_t aseed = sbxnr::fnv1a(val("FINGERPRINT") + "|" + val("SERIAL") + "|" +
                                      val("ANDROID_ID") + "|" + g_pkg);
        g_applog    = sbxnr::make_applog_ids(aseed, epoch_ms);
        g_applog_ok = true;
        LOGD("L9 applog: pkg=%s did=…%s (per-pkg, epoch=%llu)",
             g_pkg.c_str(), g_applog.did.substr(g_applog.did.size() - 4).c_str(),
             (unsigned long long)epoch_ms);
    }

    if (!orig_spg)    orig_spg    = reinterpret_cast<sbx_spg_fn>(dlsym(RTLD_DEFAULT, "__system_property_get"));
    if (!orig_spr)    orig_spr    = reinterpret_cast<sbx_spr_fn>(dlsym(RTLD_DEFAULT, "__system_property_read"));
    if (!orig_sprcb)  orig_sprcb  = reinterpret_cast<sbx_sprcb_fn>(dlsym(RTLD_DEFAULT, "__system_property_read_callback"));
    if (!orig_open)   orig_open   = reinterpret_cast<sbx_open_fn>(dlsym(RTLD_DEFAULT, "open"));
    if (!orig_openat) orig_openat = reinterpret_cast<sbx_openat_fn>(dlsym(RTLD_DEFAULT, "openat"));
    if (!orig_fopen)  orig_fopen  = reinterpret_cast<sbx_fopen_fn>(dlsym(RTLD_DEFAULT, "fopen"));

    int libs = sbx_register_across_libs(api);
    if (libs == 0) {
        LOGW("L9: no mapped .so to hook — native reads not spoofed (kill-switch keeps flag off)");
        return;
    }
    bool committed = api->pltHookCommit();
    if (!committed) {
        LOGW("L9: pltHookCommit gagal (%d libs registered) — native-read "
             "presentation dinonaktifkan agar semua surface fail-open", libs);
        g_nr_active = false;
        return;
    }
    LOGD("L9 PLT commit selesai: %d mapped libraries; late-loaded libraries "
         "remain outside confirmed coverage", libs);
    g_nr_active = orig_spg || orig_spr || orig_sprcb ||
                  orig_open || orig_openat || orig_fopen;
    if (!g_nr_active) {
        LOGW("L9: no real implementation resolvable — native reads not spoofed");
        return;
    }
    LOGD("L9 aktif (%d lib): boot_id=%s proc_version=%d meminfo=%d "
         "cpu_revision=%d "
         "[open=%p openat=%p fopen=%p spg=%p spr=%p sprcb=%p]",
         libs, g_boot_id.c_str(), g_environment_gates.proc_version ? 1 : 0,
         g_environment_gates.meminfo ? 1 : 0,
         g_environment_gates.cpu_revision ? 1 : 0,
         reinterpret_cast<void*>(orig_open),   reinterpret_cast<void*>(orig_openat),
         reinterpret_cast<void*>(orig_fopen),  reinterpret_cast<void*>(orig_spg),
         reinterpret_cast<void*>(orig_spr),    reinterpret_cast<void*>(orig_sprcb));
}

static int runtime_sdk(JNIEnv* env) {
    jclass version = env->FindClass("android/os/Build$VERSION");
    if (!version || env->ExceptionCheck()) {
        clear_jni_exception(env);
        if (version) env->DeleteLocalRef(version);
        return 0;
    }
    jfieldID field = env->GetStaticFieldID(version, "SDK_INT", "I");
    if (!field || env->ExceptionCheck()) {
        clear_jni_exception(env);
        env->DeleteLocalRef(version);
        return 0;
    }
    jint sdk = env->GetStaticIntField(version, field);
    if (env->ExceptionCheck()) {
        clear_jni_exception(env);
        sdk = 0;
    }
    env->DeleteLocalRef(version);
    return sdk > 0 ? static_cast<int>(sdk) : 0;
}

static void publish_identity(std::map<std::string, std::string>&& next) {
    g_id = std::move(next);
}

static bool read_runtime_property(const char* name, std::string& out) {
    out.clear();
    const prop_info* pi = __system_property_find(name);
    if (!pi) return false;
    sbx_sprcb_fn read_callback = reinterpret_cast<sbx_sprcb_fn>(
        dlsym(RTLD_DEFAULT, "__system_property_read_callback"));
    if (!read_callback) return false;
    struct Context {
        std::string* out;
        bool called;
    } context{&out, false};
    read_callback(
        pi,
        [](void* cookie, const char*, const char* value, uint32_t) {
            Context* context = static_cast<Context*>(cookie);
            if (!context || context->called) return;
            context->called = true;
            context->out->assign(value ? value : "");
        },
        &context);
    return context.called;
}

static bool runtime_is_stable_release() {
    std::string preview_sdk;
    std::string codename;
    return read_runtime_property("ro.build.version.preview_sdk", preview_sdk) &&
           read_runtime_property("ro.build.version.codename", codename) &&
           sbxprop::stable_release_runtime(preview_sdk, codename);
}

static void install_build_hook(JNIEnv* env) {
    jclass build = env->FindClass("android/os/Build");
    if (build && !env->ExceptionCheck()) {

        static const std::pair<const char*, const char*> f[] = {
            {"BRAND","BRAND"}, {"MANUFACTURER","MANUFACTURER"},
            {"MODEL","MODEL"}, {"DEVICE","DEVICE"}, {"PRODUCT","PRODUCT"},
            {"FINGERPRINT","FINGERPRINT"}, {"ID","ID"},
            {"DISPLAY","DISPLAY"}, {"BOOTLOADER","BOOTLOADER"},
            {"HOST","HOST"}, {"USER","USER"}, {"TYPE","TYPE"},
            {"TAGS","TAGS"}, {"RADIO","RADIO"},

            {"SERIAL","SERIAL"},
            {"SKU","SKU"},
            {"ODM_SKU","ODM_SKU"},
        };
        for (const auto& [fn, k] : f) set_str(env, build, fn, val(k));

        const std::string& butc = val("BUILD_TIME_UTC");
        if (!butc.empty()) {
            long long t = 0;
            if (sbx_parse_ll(butc, t) && t > 0)
                set_long(env, build, "TIME", (jlong)t * 1000);
        }

        env->DeleteLocalRef(build);
    } else env->ExceptionClear();

    jclass ver = env->FindClass("android/os/Build$VERSION");
    if (ver && !env->ExceptionCheck()) {
        set_str(env, ver, "RELEASE",        val("RELEASE"));
        set_str(env, ver, "INCREMENTAL",    val("INCREMENTAL"));
        set_str(env, ver, "SECURITY_PATCH", val("SECURITY_PATCH"));

        const std::string& rel = val("RELEASE");
        if (!rel.empty() && g_stable_release_runtime) {
            set_str(env, ver, "RELEASE_OR_CODENAME",        rel);
            set_str(env, ver, "RELEASE_OR_PREVIEW_DISPLAY", rel);
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

        sbxid::IdentitySnapshot snapshot;
        sbxid::ValidationContext validation;
        validation.runtime_sdk = runtime_sdk(env_);
        validation.max_blob = sandboxid::MAX_IDENTITY_BLOB;
        std::string validation_error;
        const std::string_view blob_view(
            reinterpret_cast<const char*>(blob_.data()), blob_.size());
        if (validation.runtime_sdk <= 0 ||
            !sbxid::parse_and_validate_identity(blob_view, validation, snapshot,
                                                validation_error)) {
            LOGW("identity rejected for pkg='%s': %s", pkg.c_str(),
                 validation.runtime_sdk <= 0 ? "runtime SDK unavailable"
                                             : validation_error.c_str());
            blob_.clear();
            ::close(fd);
            unload();
            return;
        }

        validated_identity_ = std::move(snapshot.values);
        blob_.clear();
        active_ = true;
        pkg_    = pkg;
        if (api_->exemptFd(fd)) {
            comp_fd_ = fd;
        } else {
            LOGW("exemptFd(fd=%d) returned false; closing companion socket before "
                 "specialization and disabling mount/hide for pkg='%s'", fd, pkg.c_str());
            ::close(fd);
        }

        LOGD("target active (%u B) [%s]", len, SBX_VARIANT_TAG);
        LOGD("target pkg='%s'", pkg.c_str());
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!active_) return;

        publish_identity(std::move(validated_identity_));
        g_stable_release_runtime = runtime_is_stable_release();
        g_build_replacements = 0;
        g_build_failures = 0;
        LOGD("validated identity: %zu keys", g_id.size());

        install_build_hook(env_);
        LOGD("Build mutation complete: replacements=%d failures=%d", g_build_replacements,
             g_build_failures);
        install_prop_hook(api_, env_);
        install_prop_handle_hooks(api_, env_);
        install_leak_sensors(api_, env_);
        install_uptime_hook(api_, env_);
        g_pkg = pkg_;
        install_native_read_hooks(api_);
#ifdef SBX_DEBUG
        for (auto& kv : g_id) LOGD("  [id] %s = %s", kv.first.c_str(), kv.second.c_str());
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
    std::map<std::string, std::string> validated_identity_;

    void unload() {
        if (api_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

    void parse_blob() = delete;
};

REGISTER_ZYGISK_MODULE(SandboxID)

extern "C" void sandboxid_companion(int client);
REGISTER_ZYGISK_COMPANION(sandboxid_companion)
