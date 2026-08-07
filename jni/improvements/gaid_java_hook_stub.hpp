// gaid_java_hook_stub.hpp
// v2.1.2: renamed namespace ttfix:: -> tt:: for consistency.
//
// OPTIONAL / FUTURE WORK — stub for LSPlant-based Java-side GAID hook.
// The pack currently runs via settings_secure.xml route (route #2).
// This is a placeholder for the eventual Java-hook backup path.

#pragma once

#include <jni.h>
#include <string>

namespace tt {

// Placeholder — real body needs LSPlant.
inline bool hook_gaid_info(JNIEnv* /*env*/, const std::string& /*gaid_uuid*/) {
    // TODO: replace with actual LSPlant hook implementation.
    return false;
}

}  // namespace tt
