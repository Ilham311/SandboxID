

#pragma once

#include <jni.h>
#include <string>
#include <sys/system_properties.h>

#ifndef TT_L2_STRICT
#define TT_L2_STRICT 1
#endif

#ifndef LOGD
#define LOGD(...) ((void)0)
#endif

namespace tt {

inline jstring prop_l2_fallback(JNIEnv* env, const std::string& key, jstring j_def) {
#if TT_L2_STRICT
    LOGD("L2 MISS-STRICT '%s' -> java default (no leak)", key.c_str());
    (void)env;
    return j_def;
#else
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(key.c_str(), buf) > 0) {
        LOGD("L2 LEAK '%s' -> '%s' (unhooked, real value)", key.c_str(), buf);
        return env->NewStringUTF(buf);
    }
    return j_def;
#endif
}

}
