#pragma once

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
#include <vector>
#include <utility>
#include <sstream>
#include <signal.h>
#include <ctime>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include "zygisk.hpp"
#include "config.hpp"
#include "native_read.hpp"
#include "hw_naming.hpp"
#include "raii.hpp"
#include "jni_string.hpp"

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

// ---- Shared state (defined in module.cpp) ----
extern std::map<std::string, std::string> g_identity;
extern std::string g_pkg;

// ---- Shared helpers (defined in split TUs) ----
extern const std::string& val(const std::string& k);
extern bool spoof_prop_value(const std::string& k, std::string& out);

// ---- Inline helpers used across TUs ----

// Props that name a specific OEM, and so must be hidden unless the persona IS
// that OEM. These cannot live in should_hide_prop() (native/native_read.cpp):
// that function is pure and has no access to the identity, and it is also
// compiled into the CLI. The JNI layer is where the persona is known.
//
// Measured on the reference device: ro.gfx.driver.0=com.xiaomi.ugd and
// ro.appsflyer.preinstall.path=/data/miui/pai/pre_install.appsflyer. A target
// app read both and got the real Xiaomi/MIUI values back while every other
// surface showed the OPPO persona.
inline bool sbx_oem_prop_hidden(const char* name) {
    if (!name) return false;
    const std::string& man = val("MANUFACTURER");
    const std::string& brd = val("BRAND");
    const std::string& soc = val("SOC_MANUFACTURER");
    const std::string& mod = val("SOC_MODEL");

    const bool xiaomi = man == "Xiaomi" || man == "Redmi" || man == "POCO" ||
                        brd == "Xiaomi" || brd == "Redmi" || brd == "POCO";
    if (!xiaomi) {
        static const char* const xiaomi_only[] = {
            "ro.gfx.driver.0",              // com.xiaomi.ugd
            "ro.appsflyer.preinstall.path", // /data/miui/pai/...
        };
        for (const char* e : xiaomi_only)
            if (std::strcmp(name, e) == 0) return true;
    }

    // The Qualcomm GPU driver package only exists on a Qualcomm device, so a
    // MediaTek/Tensor/Exynos persona must not advertise one at all. When it IS
    // Qualcomm, spoof_prop_value() derives the package and wins before this is
    // reached, so hiding here cannot suppress a valid value.
    if (std::strcmp(name, "ro.gfx.driver.1") == 0)
        return !sbxhw::is_qualcomm(soc, mod);

    return false;
}

inline bool sbx_prop_hidden(const char* name) {
    if (!name) return false;
    return sbxnr::should_hide_prop(name) || sbx_oem_prop_hidden(name);
}

inline bool sbx_parse_longlong(const std::string& v, long long& out) {
    if (v.empty()) return false;
    errno = 0;
    const char* s = v.c_str();
    if (*s == '+' || *s == '-') ++s;
    if (!*s) return false;
    char* end = nullptr;
    long long n = std::strtoll(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0' || errno == ERANGE) return false;
    out = n;
    return true;
}

inline bool sbx_should_suppress_key(const std::string& k) {
    // NOTE: "log.looper.<uid>.<tid>.slow" was suppressed here for a while. It is
    // NOT a device identifier - it is AOSP's per-process slow-dispatch threshold
    // (set at runtime by LooperMonitor, present on every device regardless of
    // persona), and the only thing suppression achieved was handing the caller
    // its own default (-1 = "not configured"), which silently switched off the
    // device's main jank diagnostic for any process we touched. The real value
    // is now passed through untouched. The uid embedded in the name is not worth
    // hiding either: an app can read its own uid directly.
    if (k.compare(0, 13, "debug.watson.") == 0)
        return true;
    return false;
}

// ---- Hook install functions (defined in split TUs) ----
void install_prop_hook(Api* api, JNIEnv* env);
void install_leak_sensors(Api* api, JNIEnv* env);
void install_uptime_hook(Api* api, JNIEnv* env);
void install_native_read_hooks(Api* api);
// Reads a property without this module's spoofing (see module_hooks.cpp).
// Returns 0 and an empty value when the native hooks are not installed.
int sbx_raw_prop_get(const char* name, char* value);
void install_crash_watchdog(const std::string& pkg);
void install_build_hook(JNIEnv* env);
void request_companion_hide(int fd);