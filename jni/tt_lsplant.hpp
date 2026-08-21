#pragma once
#include <jni.h>
#include <android/log.h>
#include <string>

#ifndef SBX_LSP_TAG
#define SBX_LSP_TAG "SandboxID-L3"
#endif
#define SBX_LSP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, SBX_LSP_TAG, __VA_ARGS__)
#ifdef SBX_DEBUG
#define SBX_LSP_LOGD(...) __android_log_print(ANDROID_LOG_INFO, SBX_LSP_TAG, "[D] " __VA_ARGS__)
#else
#define SBX_LSP_LOGD(...) ((void)0)
#endif

#ifdef SBX_ENABLE_LSPLANT
#include <dobby.h>
#include <lsplant.hpp>
#include <lsparself.hpp>
#if __has_include("tt_hook_dex.h")
#include "tt_hook_dex.h"
#define SBX_HAVE_HOOK_DEX 1
#endif
#endif

namespace sbxlsp {

inline std::string g_android_id;
inline void set_android_id(const std::string& v) { g_android_id = v; }

#ifndef SBX_ENABLE_LSPLANT
inline bool available()                               { return false; }
inline bool init(JNIEnv*  )                     { return false; }
inline bool hook_android_id(JNIEnv*  ,
                            const std::string&  )  { return false; }

#else

inline bool available() { return true; }

inline void* sbx_inline_hooker(void* target, void* hooker) {
    void* origin = nullptr;
    if (DobbyHook(target, hooker, &origin) == 0) return origin;
    return nullptr;
}
inline bool sbx_inline_unhooker(void* func) {
    return DobbyDestroy(func) == 0;
}

inline bool init(JNIEnv* env) {
    if (!env) return false;
    static bool done = false, ok = false;
    if (done) return ok;
    done = true;

    static lsparself::Elf art("/libart.so");

    static const std::string kCls = "androidx.core.os.EnvCompatState";
    static const std::string kSrc = "Hc";
    static const std::string kFld = "h";

    lsplant::InitInfo info{
        .inline_hooker   = sbx_inline_hooker,
        .inline_unhooker = sbx_inline_unhooker,
        .art_symbol_resolver =
            [](std::string_view s) -> void* { return reinterpret_cast<void*>(art.getSymbAddress(s)); },
        .art_symbol_prefix_resolver =
            [](std::string_view s) -> void* { return reinterpret_cast<void*>(art.getSymbPrefixFirstAddress(s)); },
    };
    info.generated_class_name  = kCls;
    info.generated_source_name = kSrc;
    info.generated_field_name  = kFld;

    ok = lsplant::Init(env, info);
    if (!ok) SBX_LSP_LOGE("lsplant::Init failed — L3 disabled this process (L1/L2 tetap)");
    else     SBX_LSP_LOGD("lsplant::Init ok");
    return ok;
}

inline jclass  g_cb_class  = nullptr;
inline jobject g_cb_object = nullptr;
inline jobject g_backup    = nullptr;

inline jclass load_callback_class(JNIEnv* env) {
#ifndef SBX_HAVE_HOOK_DEX
    SBX_LSP_LOGE("L3: callback DEX (tt_hook_dex.h) tak ada di build ini — hook dilewati");
    return nullptr;
#else
    if (env->PushLocalFrame(16) != 0) { env->ExceptionClear(); return nullptr; }
    jobject cls = [&]() -> jobject {
        jobject bb = env->NewDirectByteBuffer((void*)tt_hook_dex, (jlong)tt_hook_dex_len);
        if (!bb || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

        jclass loaderCls = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        if (!loaderCls || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        jmethodID ctor = env->GetMethodID(loaderCls, "<init>",
            "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        if (!ctor || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

        jclass clCls = env->FindClass("java/lang/ClassLoader");
        if (!clCls || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        jmethodID getSys = env->GetStaticMethodID(clCls, "getSystemClassLoader",
            "()Ljava/lang/ClassLoader;");
        if (!getSys || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        jobject parent = env->CallStaticObjectMethod(clCls, getSys);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

        jobject loader = env->NewObject(loaderCls, ctor, bb, parent);
        if (!loader || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

        jmethodID loadClass = env->GetMethodID(clCls, "loadClass",
            "(Ljava/lang/String;)Ljava/lang/Class;");
        if (!loadClass || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        jstring name = env->NewStringUTF("androidx.core.os.EnvCompatState");
        jobject c = env->CallObjectMethod(loader, loadClass, name);
        if (!c || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        return c;
    }();

    jclass g = cls ? (jclass)env->NewGlobalRef(cls) : nullptr;
    env->PopLocalFrame(nullptr);
    return g;
#endif
}

inline bool hook_android_id(JNIEnv* env, const std::string& value) {
    if (!env) return false;
    set_android_id(value);

    if (!g_cb_class) g_cb_class = load_callback_class(env);
    if (!g_cb_class) return false;

    if (env->PushLocalFrame(16) != 0) { env->ExceptionClear(); return false; }
    bool ok = [&]() -> bool {
        jfieldID fSpoof = env->GetStaticFieldID(g_cb_class, "spoof", "Ljava/lang/String;");
        if (!fSpoof || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        jstring jval = env->NewStringUTF(value.c_str());
        env->SetStaticObjectField(g_cb_class, fSpoof, jval);

        jmethodID hCtor = env->GetMethodID(g_cb_class, "<init>", "()V");
        if (!hCtor || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        jobject hooker = env->NewObject(g_cb_class, hCtor);
        if (!hooker || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        jmethodID hId = env->GetMethodID(g_cb_class, "handle",
            "([Ljava/lang/Object;)Ljava/lang/Object;");
        if (!hId || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        jobject cb = env->ToReflectedMethod(g_cb_class, hId, JNI_FALSE);
        if (!cb || env->ExceptionCheck()) { env->ExceptionClear(); return false; }

        jclass sec = env->FindClass("android/provider/Settings$Secure");
        if (!sec || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        jmethodID mid = env->GetStaticMethodID(sec, "getString",
            "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;");
        if (!mid || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        jobject target = env->ToReflectedMethod(sec, mid, JNI_TRUE);
        if (!target || env->ExceptionCheck()) { env->ExceptionClear(); return false; }

        jobject backup = lsplant::Hook(env, target, hooker, cb);
        if (!backup || env->ExceptionCheck()) { env->ExceptionClear(); return false; }

        jfieldID fOrig = env->GetStaticFieldID(g_cb_class, "original", "Ljava/lang/reflect/Method;");
        if (!fOrig || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        env->SetStaticObjectField(g_cb_class, fOrig, backup);

        g_cb_object = env->NewGlobalRef(hooker);
        g_backup    = env->NewGlobalRef(backup);
        return true;
    }();
    env->PopLocalFrame(nullptr);

    if (ok) SBX_LSP_LOGD("L3 Settings.Secure.getString hooked; android_id -> %s", value.c_str());
    return ok;
}
#endif

}
