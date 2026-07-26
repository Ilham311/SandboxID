// Ternak TT — Path B Java method hooks via lsplant + ShadowHook.
//
// Requires build with -DTT_HAVE_LSPLANT=1 (auto-set by CMakeLists.txt when
// prebuilt/lsplant/ and jni/shadowhook/ are present after ./fetch_lsplant.sh).
// v1.1.7: switched from jmpews/Dobby to bytedance/android-inline-hook
// (ShadowHook). Dobby master stopped compiling on NDK r26d; ShadowHook is
// actively maintained and has the same call surface (`hook_func_addr` +
// `unhook`).
// Requires jni/helper_dex.h to be generated (fetch_lsplant.sh runs javac + d8
// on java_helper/TernakHookHelper.java when both are on PATH).
//
// When neither is available, this file compiles to a no-op stub that logs
// "unavailable" — the rest of the module continues to work.

#include "java_hooks.hpp"
#include <android/log.h>
#include <mutex>

#define TT_LOG_TAG "TernakTT"
#define TT_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TT_LOG_TAG, __VA_ARGS__)
#define TT_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TT_LOG_TAG, __VA_ARGS__)
#define TT_LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TT_LOG_TAG, __VA_ARGS__)

namespace ternak_tt { namespace java_hooks {

bool IsAvailable() {
#ifdef TT_HAVE_LSPLANT
    return true;
#else
    return false;
#endif
}

#ifdef TT_HAVE_LSPLANT

#include <dlfcn.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include "lsplant.hpp"
#include "shadowhook.h"

#ifdef TT_HAVE_HELPER_DEX
#include "helper_dex.h"  // provides HELPER_DEX[] + HELPER_DEX_LEN
#endif

static std::mutex g_mu;
static bool g_inited = false;
static std::map<std::string, std::string> g_ident;
static jclass g_helper_cls = nullptr;  // global ref

// v1.1.7: ShadowHook returns a stub handle from `shadowhook_hook_func_addr()`,
// and `shadowhook_unhook()` takes that stub — but LSPlant's inline_unhooker
// callback only gives us the target address. Keep a target->stub map so we
// can round-trip. (Dobby did not need this; its DobbyDestroy took the target
// directly.)
static std::mutex g_stub_mu;
static std::unordered_map<void*, void*> g_stub_map;

// ==== Native bridges called from TernakHookHelper *_h() methods ====

static jstring JNICALL Native_getSpoof(JNIEnv* env, jclass, jstring key) {
    if (!key) return nullptr;
    const char* ck = env->GetStringUTFChars(key, nullptr);
    std::string k = ck ? ck : "";
    env->ReleaseStringUTFChars(key, ck);

    // Settings.Secure lookups
    if (k == "SEC:android_id") {
        auto it = g_ident.find("ANDROID_ID");
        if (it != g_ident.end()) return env->NewStringUTF(it->second.c_str());
    }
    if (k == "SEC:bluetooth_address") {
        return env->NewStringUTF("02:00:00:00:00:00");
    }
    if (k == "SEC:bluetooth_name") {
        auto it = g_ident.find("MODEL");
        return env->NewStringUTF(it != g_ident.end() ? it->second.c_str() : "Pixel");
    }

    // Settings.Global lookups (string form)
    if (k == "GLB:development_settings_enabled") return env->NewStringUTF("0");
    if (k == "GLB:adb_enabled")                  return env->NewStringUTF("0");
    if (k == "GLB:install_non_market_apps")      return env->NewStringUTF("0");

    return nullptr;  // no spoof -> hooker falls back to backup.invoke()
}

static jlong JNICALL Native_getSpoofLong(JNIEnv* env, jclass, jstring key) {
    if (!key) return 0;
    const char* ck = env->GetStringUTFChars(key, nullptr);
    std::string k = ck ? ck : "";
    env->ReleaseStringUTFChars(key, ck);

    if (k == "UPTIME_OFFSET_MS") {
        auto it = g_ident.find("UPTIME_OFFSET_MS");
        if (it != g_ident.end()) {
            try { return std::stoll(it->second); } catch (...) {}
        }
        // Default: subtract real uptime to "reset" apparent uptime to something
        // small and consistent (spoofed value: +3600s of apparent uptime).
        return 3600000LL;
    }
    return 0;
}

// ==== Also handle GLBI (int form) — called from same native bridge but
//      through a separate spoof key prefix. Returns numeric string. ====
// Handled inside Native_getSpoof for keys starting with "GLBI:".

// ==== Init (lsplant + Dobby wiring) ====

bool Init(JNIEnv* env) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_inited) return true;

    // ShadowHook init:
    //   SHADOWHOOK_MODE_SHARED = allow chaining (safer if the target app also
    //     uses ShadowHook internally, e.g. TikTok itself).
    //   debuggable=false       = production mode, minimal overhead.
    int sh_err = shadowhook_init(SHADOWHOOK_MODE_SHARED, false);
    if (sh_err != 0) {
        TT_LOGE("Path B: shadowhook_init failed: %d (%s)",
                sh_err, shadowhook_to_errmsg(sh_err));
        return false;
    }

    lsplant::InitInfo info{
        .inline_hooker = [](void* target, void* replace) -> void* {
            void* backup = nullptr;
            void* stub = shadowhook_hook_func_addr(target, replace, &backup);
            if (stub == nullptr) {
                int e = shadowhook_get_errno();
                TT_LOGE("Path B: shadowhook_hook_func_addr(%p) failed: %d (%s)",
                        target, e, shadowhook_to_errmsg(e));
                return nullptr;
            }
            {
                std::lock_guard<std::mutex> lk2(g_stub_mu);
                g_stub_map[target] = stub;
            }
            return backup;
        },
        .inline_unhooker = [](void* func) -> bool {
            void* stub = nullptr;
            {
                std::lock_guard<std::mutex> lk2(g_stub_mu);
                auto it = g_stub_map.find(func);
                if (it == g_stub_map.end()) return false;
                stub = it->second;
                g_stub_map.erase(it);
            }
            return shadowhook_unhook(stub) == 0;
        },
        .art_symbol_resolver = [](std::string_view name) -> void* {
            static void* h = dlopen("libart.so", RTLD_LAZY);
            if (!h) return nullptr;
            std::string s(name);
            return dlsym(h, s.c_str());
        },
    };

    if (!lsplant::Init(env, info)) {
        TT_LOGE("Path B: lsplant::Init failed");
        return false;
    }
    g_inited = true;
    TT_LOGI("Path B: lsplant::Init OK (ShadowHook inline_hooker wired)");
    return true;
}

// ==== Helpers ====

static jobject GetSystemClassLoader(JNIEnv* env) {
    jclass cl_cls = env->FindClass("java/lang/ClassLoader");
    if (!cl_cls) { env->ExceptionClear(); return nullptr; }
    jmethodID mid = env->GetStaticMethodID(cl_cls, "getSystemClassLoader",
                                            "()Ljava/lang/ClassLoader;");
    if (!mid) { env->ExceptionClear(); env->DeleteLocalRef(cl_cls); return nullptr; }
    jobject loader = env->CallStaticObjectMethod(cl_cls, mid);
    env->DeleteLocalRef(cl_cls);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return loader;
}

#ifdef TT_HAVE_HELPER_DEX
static jclass LoadHelperClass(JNIEnv* env) {
    // ByteBuffer buf = ByteBuffer.wrap(HELPER_DEX)
    jbyteArray dex_arr = env->NewByteArray((jsize)HELPER_DEX_LEN);
    if (!dex_arr) { env->ExceptionClear(); return nullptr; }
    env->SetByteArrayRegion(dex_arr, 0, (jsize)HELPER_DEX_LEN,
                            reinterpret_cast<const jbyte*>(HELPER_DEX));

    jclass bb_cls = env->FindClass("java/nio/ByteBuffer");
    jmethodID wrap = env->GetStaticMethodID(bb_cls, "wrap",
                                             "([B)Ljava/nio/ByteBuffer;");
    jobject buf = env->CallStaticObjectMethod(bb_cls, wrap, dex_arr);
    env->DeleteLocalRef(dex_arr);
    env->DeleteLocalRef(bb_cls);
    if (env->ExceptionCheck() || !buf) { env->ExceptionClear(); return nullptr; }

    // InMemoryDexClassLoader loader = new InMemoryDexClassLoader(buf, parent)
    jobject parent = GetSystemClassLoader(env);
    jclass ldr_cls = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (!ldr_cls) { env->ExceptionClear(); return nullptr; }
    jmethodID ctor = env->GetMethodID(ldr_cls, "<init>",
        "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    jobject loader = env->NewObject(ldr_cls, ctor, buf, parent);
    env->DeleteLocalRef(buf);
    if (parent) env->DeleteLocalRef(parent);
    if (env->ExceptionCheck() || !loader) {
        env->ExceptionClear();
        env->DeleteLocalRef(ldr_cls);
        return nullptr;
    }

    // Class<?> cls = loader.loadClass("com.ternak.tt.TernakHookHelper")
    jmethodID load_class = env->GetMethodID(ldr_cls, "loadClass",
        "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("com.ternak.tt.TernakHookHelper");
    jclass helper = (jclass)env->CallObjectMethod(loader, load_class, name);
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(loader);
    env->DeleteLocalRef(ldr_cls);
    if (env->ExceptionCheck() || !helper) {
        env->ExceptionClear();
        return nullptr;
    }
    return (jclass)env->NewGlobalRef(helper);
}
#else
static jclass LoadHelperClass(JNIEnv*) {
    TT_LOGE("Path B: helper_dex.h not embedded (build without javac/d8)");
    return nullptr;
}
#endif

// Attempts to hook a single Java method. On success, stores backup Method in
// helper's static Method field (backup_field_name).
static bool HookOne(JNIEnv* env, jclass helper_cls,
                    const char* target_cls_name,
                    const char* target_method_name,
                    const char* target_sig,
                    bool target_is_static,
                    const char* hooker_method_name,
                    const char* hooker_sig,
                    const char* backup_field_name) {
    // Resolve target
    jclass tc = env->FindClass(target_cls_name);
    if (!tc) { env->ExceptionClear();
        TT_LOGE("Path B: FindClass %s failed", target_cls_name);
        return false;
    }
    jmethodID tmid = target_is_static
        ? env->GetStaticMethodID(tc, target_method_name, target_sig)
        : env->GetMethodID(tc, target_method_name, target_sig);
    if (!tmid) { env->ExceptionClear();
        TT_LOGE("Path B: GetMethodID %s.%s%s failed", target_cls_name,
                target_method_name, target_sig);
        env->DeleteLocalRef(tc);
        return false;
    }
    jobject target_ref = env->ToReflectedMethod(tc, tmid,
                                                 target_is_static ? JNI_TRUE : JNI_FALSE);
    env->DeleteLocalRef(tc);
    if (!target_ref) { env->ExceptionClear();
        TT_LOGE("Path B: ToReflectedMethod target %s failed", target_method_name);
        return false;
    }

    // Resolve hooker (always static on TernakHookHelper)
    jmethodID hmid = env->GetStaticMethodID(helper_cls, hooker_method_name, hooker_sig);
    if (!hmid) { env->ExceptionClear();
        TT_LOGE("Path B: GetStaticMethodID helper.%s%s failed",
                hooker_method_name, hooker_sig);
        env->DeleteLocalRef(target_ref);
        return false;
    }
    jobject hooker_ref = env->ToReflectedMethod(helper_cls, hmid, JNI_TRUE);
    if (!hooker_ref) { env->ExceptionClear();
        env->DeleteLocalRef(target_ref);
        return false;
    }

    // lsplant::Hook -> returns backup Method
    jobject backup = lsplant::Hook(env, target_ref, nullptr, hooker_ref);
    env->DeleteLocalRef(target_ref);
    env->DeleteLocalRef(hooker_ref);
    if (!backup) { env->ExceptionClear();
        TT_LOGE("Path B: lsplant::Hook %s.%s failed", target_cls_name, target_method_name);
        return false;
    }

    // Store backup into helper's static Method field
    jfieldID fid = env->GetStaticFieldID(helper_cls, backup_field_name,
                                          "Ljava/lang/reflect/Method;");
    if (!fid) { env->ExceptionClear();
        TT_LOGE("Path B: GetStaticFieldID helper.%s failed", backup_field_name);
        return false;
    }
    env->SetStaticObjectField(helper_cls, fid, backup);
    env->DeleteLocalRef(backup);
    TT_LOGI("Path B: hooked %s.%s", target_cls_name, target_method_name);
    return true;
}

void InstallAll(JNIEnv* env, const std::map<std::string, std::string>& identity) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_inited) {
        TT_LOGE("Path B: InstallAll called before Init OK");
        return;
    }
    g_ident = identity;

    if (!g_helper_cls) {
        g_helper_cls = LoadHelperClass(env);
        if (!g_helper_cls) {
            TT_LOGE("Path B: helper class load failed — no hooks installed");
            return;
        }

        // Register native bridges on helper
        JNINativeMethod natives[] = {
            {(char*)"nativeGetSpoof",     (char*)"(Ljava/lang/String;)Ljava/lang/String;",
             (void*)Native_getSpoof},
            {(char*)"nativeGetSpoofLong", (char*)"(Ljava/lang/String;)J",
             (void*)Native_getSpoofLong},
        };
        if (env->RegisterNatives(g_helper_cls, natives, 2) != 0) {
            TT_LOGE("Path B: RegisterNatives on helper failed");
            env->ExceptionClear();
            return;
        }
        TT_LOGI("Path B: helper class loaded + native bridges registered");
    }

    int ok = 0, fail = 0;

    // 1. Settings.Secure.getString(ContentResolver, String) -> String
    if (HookOne(env, g_helper_cls,
                "android/provider/Settings$Secure",
                "getString",
                "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",
                true,
                "secureGetString_h",
                "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",
                "secureGetString_bak")) ok++; else fail++;

    // 2. Settings.Global.getString(ContentResolver, String) -> String
    if (HookOne(env, g_helper_cls,
                "android/provider/Settings$Global",
                "getString",
                "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",
                true,
                "globalGetString_h",
                "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",
                "globalGetString_bak")) ok++; else fail++;

    // 3. Settings.Global.getInt(ContentResolver, String, int) -> int
    if (HookOne(env, g_helper_cls,
                "android/provider/Settings$Global",
                "getInt",
                "(Landroid/content/ContentResolver;Ljava/lang/String;I)I",
                true,
                "globalGetInt_h",
                "(Landroid/content/ContentResolver;Ljava/lang/String;I)I",
                "globalGetInt_bak")) ok++; else fail++;

    // 4. SystemClock.uptimeMillis() -> long
    if (HookOne(env, g_helper_cls,
                "android/os/SystemClock",
                "uptimeMillis",
                "()J",
                true,
                "uptimeMillis_h",
                "()J",
                "uptimeMillis_bak")) ok++; else fail++;

    // 5. SystemClock.elapsedRealtime() -> long
    if (HookOne(env, g_helper_cls,
                "android/os/SystemClock",
                "elapsedRealtime",
                "()J",
                true,
                "elapsedRealtime_h",
                "()J",
                "elapsedRealtime_bak")) ok++; else fail++;

    TT_LOGI("Path B: InstallAll finished (%d ok, %d fail)", ok, fail);
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
