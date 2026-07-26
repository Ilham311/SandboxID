#include "java_hooks.hpp"
#include <android/log.h>

#ifdef TT_HAVE_LSPLANT
#include <dlfcn.h>
#include <string>
#include <string_view>
#include "lsplant.hpp"
#endif

#define TT_LOG_TAG "TernakTT"
#define TT_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TT_LOG_TAG, __VA_ARGS__)
#define TT_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TT_LOG_TAG, __VA_ARGS__)

namespace ternak_tt { namespace java_hooks {

bool IsAvailable() {
#ifdef TT_HAVE_LSPLANT
    return true;
#else
    return false;
#endif
}

#ifdef TT_HAVE_LSPLANT

static bool g_inited = false;

bool Init(JNIEnv* env) {
    if (g_inited) return true;
    lsplant::InitInfo info{
        .inline_hooker       = [](auto*, auto*) -> void* { return nullptr; },
        .inline_unhooker     = [](auto*) -> bool { return true; },
        .art_symbol_resolver = [](std::string_view name) -> void* {
            static void* h = dlopen("libart.so", RTLD_LAZY);
            std::string s(name);
            return h ? dlsym(h, s.c_str()) : nullptr;
        },
    };
    if (!lsplant::Init(env, info)) {
        TT_LOGE("Path B: lsplant::Init failed");
        return false;
    }
    g_inited = true;
    TT_LOGI("Path B: java_hooks Init OK (Dobby integration pending v1.1.1)");
    return true;
}

void InstallAll(JNIEnv*, const std::map<std::string, std::string>&) {
    if (!g_inited) return;
    TT_LOGI("Path B: InstallAll scaffold (0 hooks; v1.1.1 wires Settings.Secure + MediaDrm + Locale + TimeZone + SystemClock)");
}

#else

bool Init(JNIEnv*) {
    TT_LOGI("Path B: unavailable (built without lsplant; run ./fetch_lsplant.sh + rebuild)");
    return false;
}

void InstallAll(JNIEnv*, const std::map<std::string, std::string>&) {
    // no-op when Path B not compiled in
}

#endif

}} // namespace ternak_tt::java_hooks
