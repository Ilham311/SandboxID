#pragma once

#include <jni.h>
#include <functional>
#include <string_view>

namespace lsplant {

struct InitInfo {
    std::function<void*(void*, void*)>       inline_hooker;
    std::function<bool(void*)>               inline_unhooker;
    std::function<void*(std::string_view)>   art_symbol_resolver;
    std::string_view                         generated_class_name;
    std::string_view                         generated_source_name;
    std::string_view                         generated_field_name;
};

inline bool    Init(JNIEnv*, const InitInfo&)              { return true; }
inline jobject Hook(JNIEnv*, jobject, jobject, jobject)    { return nullptr; }
inline bool    Deoptimize(JNIEnv*, jobject)               { return true; }

}
