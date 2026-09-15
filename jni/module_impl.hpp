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
#include <atomic>
#include "zygisk.hpp"
#include "config.hpp"
#include "native_read.hpp"
#include "raii.hpp"

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
inline bool sbx_prop_hidden(const char* name) {
    return name && sbxnr::should_hide_prop(name);
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
    if (k.size() >= 11 + 5 &&
        k.compare(0, 11, "log.looper.") == 0 &&
        k.compare(k.size() - 5, 5, ".slow") == 0)
        return true;
    if (k.compare(0, 13, "debug.watson.") == 0)
        return true;
    return false;
}

// ---- Hook install functions (defined in split TUs) ----
void install_prop_hook(Api* api, JNIEnv* env);
void install_leak_sensors(Api* api, JNIEnv* env);
void install_uptime_hook(Api* api, JNIEnv* env);
void install_native_read_hooks(Api* api);
void install_crash_watchdog(const std::string& pkg);
void install_build_hook(JNIEnv* env);
void request_companion_mounts(int fd);
void request_companion_hide(int fd);