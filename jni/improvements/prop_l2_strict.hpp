// ternak_tt v2 — prop_l2_strict.hpp
// Item #6: strict L2 hook_prop_get — no __system_property_get leak fallback.
//
// USAGE (in main.cpp, replace the fallback tail of hook_prop_get):
//   #include "improvements/prop_l2_strict.hpp"
//   ...
//   auto it = map.find(k);
//   if (it != map.end()) { ... return spoofed ... }
//   auto sit = static_defaults.find(k);
//   if (sit != static_defaults.end()) { ... return static ... }
//
//   // BEFORE (v1 leak):
//   //   char buf[PROP_VALUE_MAX] = {0};
//   //   if (__system_property_get(k.c_str(), buf) > 0)
//   //       return env->NewStringUTF(buf);
//   //   return j_def;
//
//   // AFTER (v2 strict):
//   return tt::prop_l2_fallback(env, k, j_def);
//
// The strict variant never leaks a real property value. If the key isn't in
// the allow-list, we return whatever Java default the caller passed — which
// is what Android already does when a property doesn't exist.
//
// Compile with -DTT_L2_STRICT=1 for strict mode (default). Define =0 to
// preserve v1 leak behavior for compat testing.

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

// Returns j_def (or nullptr if j_def is null) when strict mode is on.
// In non-strict mode, reads real prop and returns it (v1 behavior, for A/B).
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

}  // namespace tt
