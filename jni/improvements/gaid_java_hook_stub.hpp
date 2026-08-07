

#pragma once

#include <jni.h>
#include <string>

namespace ttfix {

inline bool hook_gaid_info(JNIEnv*  , const std::string&  ) {

    return false;
}

}
