#pragma once

#include <jni.h>
#include <functional>
#include <string_view>

namespace lsplant {

struct InitInfo {
    std::function<void*(void*, void*)>       inline_hooker;
    std::function<bool(void*)>               inline_unhooker;
    std::function<void*(std::string_view)>   art_symbol_resolver;
    std::function<void*(std::string_view)>   art_symbol_prefix_resolver;  // v6.4+
    std::string_view                         generated_class_name;
    std::string_view                         generated_source_name;
    std::string_view                         generated_field_name;
    std::string_view                         generated_method_name;       // v6.4+
};

// [[nodiscard]] meniru signature asli LSPlant v6.4 supaya validate.sh lokal
// benar-benar menguji cast (void) pada hasil yang diabaikan.
[[nodiscard]] inline bool    Init(JNIEnv*, const InitInfo&)           { return true; }
[[nodiscard]] inline jobject Hook(JNIEnv*, jobject, jobject, jobject) { return nullptr; }
[[nodiscard]] inline bool    Deoptimize(JNIEnv*, jobject)            { return true; }

}
