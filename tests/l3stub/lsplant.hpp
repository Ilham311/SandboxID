#pragma once
// Minimal LSPlant v2.0 stub for host syntax-checking jni/sbx_lsplant.hpp under
// -DSBX_ENABLE_LSPLANT without an NDK/AAR. The InitInfo member ORDER matches the
// designated-initializer order used by sbx_lsplant.hpp::init() (C++20 requires
// designators in declaration order), so a drift there is caught host-side.
// Deliberately has NO art_symbol_prefix_resolver — that field is post-v2.0, and
// re-adding a designated initializer for it would (correctly) fail here.
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

}  // namespace lsplant
