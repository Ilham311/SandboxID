

#pragma once

#include <jni.h>
#include <string>

namespace tt {

inline bool hook_gaid_info(JNIEnv*  , const std::string&  ) {

    return false;
}

}
