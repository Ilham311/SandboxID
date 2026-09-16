#pragma once

/* Host shim for <android/log.h>.
 *
 * The host suites compile the real jni/module_hooks.cpp, which includes the
 * real module_impl.hpp, which includes <android/log.h>. On a device that is
 * bionic's logging header; on a stock Linux host (notably GitHub's ubuntu
 * runners) it does not exist, and the suite dies at the #include before a
 * single check can run.
 *
 * The surface the module actually uses is tiny: four priority constants, and
 * __android_log_print itself. LOGD compiles to nothing under -DNDEBUG, so on
 * a release-style host build the only live callers are LOGW/LOGI/LOGE, and a
 * logcat-less host has nowhere to send them anyway — stubbing to a no-op is
 * the honest behaviour, not a silence.
 *
 * run.sh adds this directory to the include path ONLY when the real
 * <android/log.h> is absent, so Termux and any box with the NDK headers keep
 * compiling against the real thing. */

enum {
    ANDROID_LOG_UNKNOWN = 0,
    ANDROID_LOG_DEFAULT,
    ANDROID_LOG_VERBOSE,
    ANDROID_LOG_DEBUG,
    ANDROID_LOG_INFO,
    ANDROID_LOG_WARN,
    ANDROID_LOG_ERROR,
    ANDROID_LOG_FATAL,
    ANDROID_LOG_SILENT,
};

/* Declared only. The definition lives in tests/host/sbx_hook_sm.cpp, which
 * stubs it to a no-op for the host build — declared extern "C" there, so the
 * linkage must match here or the two conflict. */
extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
