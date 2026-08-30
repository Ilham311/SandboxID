#pragma once
// Minimal android/log.h stub for host syntax-checking the L3 layer. Never linked.
#include <cstdarg>

enum {
    ANDROID_LOG_INFO  = 4,
    ANDROID_LOG_ERROR = 6,
};

static inline int __android_log_print(int, const char*, const char*, ...) { return 0; }
