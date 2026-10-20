#pragma once
//
// L3: Java-method hooks through LSPlant.
//
// Why this layer exists at all: the property-based layers (L2 SystemProperties,
// L7 typed getters, L9 libc __system_property_*) can only reach values that are
// stored in properties. The identifiers below are not:
//
//   android_id      -> Settings.Secure, read through a ContentProvider
//   SERIAL          -> Build.getSerial(), gated behind READ_PHONE_STATE
//   IMEI/MEID/IMSI  -> delivered over Binder by the telephony system service
//   ICCID           -> same, from the SIM
//   Widevine id     -> MediaDrm.getPropertyByteArray("deviceUniqueId")
//   WiFi/BT MAC     -> WifiInfo/BluetoothAdapter, backed by the kernel + service
//
// Every one of those is a Java call in the app process, so an in-process Java
// hook is the only place a spoof can be applied. That is what LSPlant provides.
//
// FAIL-SOFT CONTRACT: install_all() returning false is normal and must never
// take the process down. If LSPlant cannot initialise on this ART build (a
// stripped symbol, an unknown API level, a hooker that refuses), every target is
// skipped and L1/L2/L9 keep working. Nothing here may throw across JNI.

#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>
#include <cstdint>

// The LSPlant headers MUST be included here, at global scope, and not inside
// the sbxlsp namespace below. lsplant.hpp opens `namespace lsplant { inline
// namespace v2 { ... } }`; including it inside another namespace nests those,
// and every call then references sbxlsp::lsplant::v2::Init instead of
// ::lsplant::v2::Init. The definition lives in lsplant.cc under the real name,
// so the module links with three unresolved symbols:
//   sbxlsp::lsplant::v2::Init / Hook / Deoptimize
// A shared library may carry undefined symbols, so this only shows up as a hard
// link error on some toolchains (the NDK build) and as a silent, guaranteed
// runtime failure everywhere else — the hooks would never install.
//
// Guarded: an L3=0 build has no include path for either header, and must stay
// compilable as the no-op stub below.
#ifdef SBX_ENABLE_LSPLANT
#include <dobby.h>
#include <lsplant.hpp>
#endif

#ifndef SBX_LSP_TAG
#define SBX_LSP_TAG "SandboxID-L3"
#endif
#define SBX_LSP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, SBX_LSP_TAG, __VA_ARGS__)
#ifdef SBX_DEBUG
#define SBX_LSP_LOGD(...) __android_log_print(ANDROID_LOG_INFO, SBX_LSP_TAG, "[D] " __VA_ARGS__)
#else
#define SBX_LSP_LOGD(...) ((void)0)
#endif

namespace sbxlsp {

// Values the module feeds to the L3 hooks. module.cpp fills these from the
// identity blob (val()) and from the same AppLog derivation the shell side seeds
// into the app's SharedPreferences, so what the SDK reads from disk and what the
// register body is rewritten to carry are the same persona.
struct HookValues {
    std::string android_id;
    std::string serial;
    std::string wifi_mac;
    std::string op_num;     // gsm operator numeric (MCC+MNC), e.g. "51010"
    std::string op_alpha;   // operator display name, e.g. "Telkomsel"
    std::string op_iso;     // operator country iso, e.g. "id"
    std::string gaid;       // advertising id, synthesized from seed
    std::string openudid;   // persona openudid (same value rotate_ids.sh seeds)
    std::string clientudid; // persona clientudid
    std::string cdid;       // persona cdid
    uint64_t    seed = 0;   // fnv1a(FINGERPRINT|SERIAL|ANDROID_ID)
};

#ifndef SBX_ENABLE_LSPLANT
// ---- Build without LSPlant: everything compiles to no-ops (default build) ----
inline bool available()                              { return false; }
inline bool init(JNIEnv*)                            { return false; }
inline bool install_all(JNIEnv*, const HookValues&)  { return false; }

#else
// ============================ LSPlant-enabled build ============================
//
// PINNED DEPENDENCY: LSPlant v6.4 (7217ac6f41e2bda549e4acb54e632abb03e5ccaf),
// built from source as a STATIC library and linked into libsandboxid.so.
//
// Why static and not the prebuilt `lsplant-standalone` AAR: Zygisk loads exactly
// one shared object (zygisk/<abi>.so) into the app process, and the app process
// cannot read /data/adb/modules (SELinux), so a second .so cannot be dlopen()ed
// from the module directory at runtime. Linking the static target keeps the
// whole module in a single file with no packaging or search-path requirement.
//
// Why v6.4 and not master: master moved to a C++23 modules build
// (cmake_minimum_required 3.28, `find_package(cxx CONFIG)`, .ixx units), which
// would force NDK r30 + Ninja into the whole build for no functional gain here.
// v6.4 is a plain C++20 single-TU build and supports API 21-35, which covers the
// test device (API 35). The known API-36 breakage (LSPosed/LSPlant#179:
// AdjustThreadVisibilityCounter absent -> SIGSEGV in ClassLinker::SetupClass)
// does NOT apply on API 35; on an API 36 device init() simply fails and the
// fail-soft contract above keeps the rest of the module running.
//
// Dobby is still needed: LSPlant ships no inline hook engine and takes one
// through InitInfo. It is pinned and built from source as `dobby_static`.
// (<dobby.h> and <lsplant.hpp> are included at the top of this file, outside
// namespace sbxlsp — see the comment there.)
#include "sbx_elf.hpp"           // in-tree libart.so symbol resolver
#include "sbx_ident_synth.hpp"   // sbxid::synth_all + sbxnr:: primitives
#if __has_include("hook_dex.h")
#include "hook_dex.h"
#define SBX_HAVE_HOOK_DEX 1
#endif

inline bool available() { return true; }

inline void* sbx_inline_hooker(void* target, void* hooker) {
    void* origin = nullptr;
    // dobby_dummy_func_t is `void (*)()` — an untyped slot, the same convention
    // upstream uses for DobbyHook's replace/origin pair.
    if (DobbyHook(target, reinterpret_cast<dobby_dummy_func_t>(hooker),
                  reinterpret_cast<dobby_dummy_func_t*>(&origin)) == 0) return origin;
    return nullptr;
}
inline bool sbx_inline_unhooker(void* func) { return DobbyDestroy(func) == 0; }

// ---- which spoof value a target returns ----
enum ValId {
    V_NONE = 0, V_ZERO, V_ANDROID_ID, V_SERIAL, V_IMEI, V_MEID, V_IMSI, V_ICCID,
    V_OP_NUM, V_OP_ALPHA, V_OP_ISO, V_WIFI_MAC, V_BT_ADDR, V_WIDEVINE,
    V_GAID, V_OPENUDID, V_CLIENTUDID, V_CDID
};

// Callback behaviour, mirrored by the constants in jni/SandboxIDHook.java.
enum Mode {
    M_VALUE        = 0,  // return the spoof value, do not call the original
    M_FORCE_FALSE  = 1,  // overwrite one Boolean argument, then call the original
    M_REWRITE_BODY = 2,  // rewrite a byte[] request body, then call the original
    M_OBSERVE      = 3,  // call the original, then report the loaded Class here
    M_ZERO_NUMERIC = 4,  // call the original; return "0" for an all-digit long key
};

// ret_type: 0 String, 1 byte[]
struct HookSpec {
    const char* cls;
    const char* name;
    const char* sig;
    bool        is_static;
    int         mode;
    int         key_index;   // absolute index into args[] (incl. receiver), or -1
    const char* key_match;   // required value at key_index, or nullptr
    bool        key_contains;
    int         ret_type;    // 0 String, 1 byte[]
    int         val_id;
    int         force_index; // M_FORCE_FALSE
    int         body_index;  // M_REWRITE_BODY
    int         url_index;   // M_REWRITE_BODY
    const char* url_match;   // M_REWRITE_BODY
};

// Per-process resolved state, shared by every hook in this process.
struct State {
    jclass    cb_class      = nullptr;
    jmethodID cb_ctor       = nullptr;
    jmethodID cb_handle     = nullptr;
    jobject   cb_reflected  = nullptr;
    jfieldID  f_mode = nullptr, f_isStatic = nullptr, f_keyIdx = nullptr,
              f_keyMatch = nullptr, f_keyContains = nullptr, f_retType = nullptr,
              f_sval = nullptr, f_bval = nullptr, f_backup = nullptr,
              f_forceIdx = nullptr, f_bodyIdx = nullptr, f_urlIdx = nullptr,
              f_urlMatch = nullptr, f_openudid = nullptr, f_clientudid = nullptr,
              f_cdid = nullptr;
    std::vector<jobject> keep;   // global refs: GC anchor for hookers + backups
    HookValues vals;
    sbxid::SynthIds ids;
    std::string wifi, bt;
};
inline State g;

// ---- callback class (androidx.core.os.EnvCompatState) loaded from embedded DEX ----
inline jclass load_callback_class(JNIEnv* env) {
#ifndef SBX_HAVE_HOOK_DEX
    SBX_LSP_LOGE("L3: callback DEX (hook_dex.h) tak ada di build ini - hook dilewati");
    return nullptr;
#else
    if (env->PushLocalFrame(16) != 0) { env->ExceptionClear(); return nullptr; }
    jobject cls = [&]() -> jobject {
        jobject bb = env->NewDirectByteBuffer((void*)hook_dex, (jlong)hook_dex_len);
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
    jclass g_cls = cls ? (jclass)env->NewGlobalRef(cls) : nullptr;
    env->PopLocalFrame(nullptr);
    return g_cls;
#endif
}

// Installed by resolve_callback_members(); called from the Java observer hook
// with the class name the app asked for and the Class it got.
inline void sbx_observe(JNIEnv* env, jclass, jstring name, jclass cls);
inline const HookSpec* tt_specs(size_t& n);

inline bool resolve_callback_members(JNIEnv* env) {
    if (!g.cb_class) return false;
    g.cb_ctor   = env->GetMethodID(g.cb_class, "<init>", "()V");
    g.cb_handle = env->GetMethodID(g.cb_class, "handle",
                                   "([Ljava/lang/Object;)Ljava/lang/Object;");
    if (!g.cb_ctor || !g.cb_handle || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    jobject refl = env->ToReflectedMethod(g.cb_class, g.cb_handle, JNI_FALSE);
    if (!refl || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    g.cb_reflected = env->NewGlobalRef(refl);

    g.f_mode       = env->GetFieldID(g.cb_class, "mode",         "I");
    g.f_isStatic   = env->GetFieldID(g.cb_class, "isStatic",     "Z");
    g.f_keyIdx     = env->GetFieldID(g.cb_class, "keyArgIndex",  "I");
    g.f_keyMatch   = env->GetFieldID(g.cb_class, "keyMatch",     "Ljava/lang/String;");
    g.f_keyContains= env->GetFieldID(g.cb_class, "keyContains",  "Z");
    g.f_retType    = env->GetFieldID(g.cb_class, "retType",      "I");
    g.f_sval       = env->GetFieldID(g.cb_class, "sval",         "Ljava/lang/String;");
    g.f_bval       = env->GetFieldID(g.cb_class, "bval",         "[B");
    g.f_backup     = env->GetFieldID(g.cb_class, "backup",       "Ljava/lang/reflect/Method;");
    g.f_forceIdx   = env->GetFieldID(g.cb_class, "forceArgIndex","I");
    g.f_bodyIdx    = env->GetFieldID(g.cb_class, "bodyArgIndex", "I");
    g.f_urlIdx     = env->GetFieldID(g.cb_class, "urlArgIndex",  "I");
    g.f_urlMatch   = env->GetFieldID(g.cb_class, "urlMatch",     "Ljava/lang/String;");
    g.f_openudid   = env->GetFieldID(g.cb_class, "openudid",     "Ljava/lang/String;");
    g.f_clientudid = env->GetFieldID(g.cb_class, "clientudid",   "Ljava/lang/String;");
    g.f_cdid       = env->GetFieldID(g.cb_class, "cdid",         "Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }

    static const JNINativeMethod NATIVE[] = {
        { (char*)"observe", (char*)"(Ljava/lang/String;Ljava/lang/Class;)V",
          (void*)+[](JNIEnv* e, jclass thiz, jstring n, jclass c) {
              sbx_observe(e, thiz, n, c);
          } },
    };
    if (env->RegisterNatives(g.cb_class, NATIVE, 1) != 0) {
        env->ExceptionClear();
        SBX_LSP_LOGD("L3: RegisterNatives(observe) gagal - jalur TikTok nonaktif");
    }

    // The observer filters on this list before making the JNI call; keep it in
    // step with tt_specs() so no class is filtered out before it is matched.
    jclass strCls = env->FindClass("java/lang/String");
    jfieldID f_watch = env->GetStaticFieldID(g.cb_class, "watch", "[Ljava/lang/String;");
    if (strCls && f_watch && !env->ExceptionCheck()) {
        size_t tn = 0;
        const HookSpec* ts = tt_specs(tn);
        jobjectArray arr = env->NewObjectArray((jsize)tn, strCls, nullptr);
        if (arr) {
            for (size_t i = 0; i < tn; ++i) {
                jstring s = env->NewStringUTF(ts[i].cls);
                if (s) env->SetObjectArrayElement(arr, (jsize)i, s);
            }
            env->SetStaticObjectField(g.cb_class, f_watch, arr);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    return g.f_mode && g.f_isStatic && g.f_keyIdx && g.f_keyMatch && g.f_keyContains &&
           g.f_retType && g.f_sval && g.f_bval && g.f_backup && g.f_forceIdx &&
           g.f_bodyIdx && g.f_urlIdx && g.f_urlMatch && g.f_openudid &&
           g.f_clientudid && g.f_cdid;
}

inline bool init(JNIEnv* env) {
    if (!env) return false;
    static bool done = false, ok = false;
    if (done) return ok;
    done = true;

    // Resolved once, lazily: the ELF walk parses both .dynsym and (when present)
    // the on-disk .symtab, which is not free.
    static sbxelf::Elf art("libart.so");
    if (!art.ok()) {
        SBX_LSP_LOGE("libart.so not found in the link map - L3 disabled this process");
        return false;
    }

    // Names chosen to blend into an ordinary androidx frame rather than advertise
    // a hook: this class name is what LSPlant bakes into the generated stub that
    // shows up in stack traces and heap dumps.
    static const std::string kCls = "androidx.core.os.HandlerCompatRef";
    static const std::string kSrc = "Hc";
    static const std::string kFld = "h";

    lsplant::InitInfo info{
        .inline_hooker   = sbx_inline_hooker,
        .inline_unhooker = sbx_inline_unhooker,
        // getSymbAddressOrUniq, not getSymbAddress: a release ROM built with
        // LTO appends `.__uniq.<hash>` to file-local names, so the exact name
        // LSPlant asks for is not the name the ROM has. The prefix resolver
        // below cannot cover this case because LSPlant's own prefix already
        // carries a different trailing type signature.
        .art_symbol_resolver =
            [](std::string_view s) -> void* { return art.getSymbAddressOrUniq(s); },
        .art_symbol_prefix_resolver =
            [](std::string_view s) -> void* { return art.getSymbPrefixFirstAddress(s); },
    };
    info.generated_class_name  = kCls;
    info.generated_source_name = kSrc;
    info.generated_field_name  = kFld;

    ok = lsplant::Init(env, info);
    if (!ok) SBX_LSP_LOGE("lsplant::Init failed - L3 disabled this process (L1/L2/L9 tetap)");
    else     SBX_LSP_LOGD("lsplant::Init ok (%zu libart symbols)", art.symbol_count());
    return ok;
}

inline std::string sbx_mac_upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
    return s;
}
inline jbyteArray sbx_hex_to_jbytes(JNIEnv* env, const std::string& hex) {
    size_t n = hex.size() / 2;
    jbyteArray a = env->NewByteArray((jsize)n);
    if (!a) return nullptr;
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    std::vector<jbyte> buf(n);
    for (size_t i = 0; i < n; ++i)
        buf[i] = (jbyte)((hv(hex[2 * i]) << 4) | hv(hex[2 * i + 1]));
    env->SetByteArrayRegion(a, 0, (jsize)n, buf.data());
    return a;
}

inline std::string sbx_value_for(int val_id, const HookValues& v,
                                 const sbxid::SynthIds& ids,
                                 const std::string& wifi, const std::string& bt) {
    switch (val_id) {
        case V_ZERO:       return std::string("0");
        case V_ANDROID_ID: return v.android_id;
        case V_SERIAL:     return v.serial;
        case V_IMEI:       return ids.imei;
        case V_MEID:       return ids.meid;
        case V_IMSI:       return ids.imsi;
        case V_ICCID:      return ids.iccid;
        case V_OP_NUM:     return v.op_num;
        case V_OP_ALPHA:   return v.op_alpha;
        case V_OP_ISO:     return v.op_iso;
        case V_WIFI_MAC:   return wifi;
        case V_BT_ADDR:    return bt;
        case V_GAID:       return v.gaid;
        case V_OPENUDID:   return v.openudid;
        case V_CLIENTUDID: return v.clientudid;
        case V_CDID:       return v.cdid;
        default:           return std::string();
    }
}

// Hook one spec against an already-resolved class. Used both for the framework
// targets (FindClass by name) and for the TikTok targets (handed over by the
// class-load observer). Fail-soft: any miss returns false and is skipped.
inline bool hook_one_class(JNIEnv* env, jclass cls, const HookSpec& sp) {
    if (!cls) return false;
    if (env->PushLocalFrame(24) != 0) { env->ExceptionClear(); return false; }
    bool ok = [&]() -> bool {
        jmethodID mid = sp.is_static ? env->GetStaticMethodID(cls, sp.name, sp.sig)
                                     : env->GetMethodID(cls, sp.name, sp.sig);
        if (!mid || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        jobject target = env->ToReflectedMethod(cls, mid, sp.is_static ? JNI_TRUE : JNI_FALSE);
        if (!target || env->ExceptionCheck()) { env->ExceptionClear(); return false; }

        jobject hooker = env->NewObject(g.cb_class, g.cb_ctor);
        if (!hooker || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        env->SetIntField(hooker, g.f_mode, sp.mode);
        env->SetBooleanField(hooker, g.f_isStatic, sp.is_static ? JNI_TRUE : JNI_FALSE);
        env->SetIntField(hooker, g.f_keyIdx, sp.key_index);
        env->SetIntField(hooker, g.f_retType, sp.ret_type);
        env->SetBooleanField(hooker, g.f_keyContains, sp.key_contains ? JNI_TRUE : JNI_FALSE);
        env->SetIntField(hooker, g.f_forceIdx, sp.force_index);
        env->SetIntField(hooker, g.f_bodyIdx, sp.body_index);
        env->SetIntField(hooker, g.f_urlIdx, sp.url_index);
        if (sp.key_match) {
            jstring km = env->NewStringUTF(sp.key_match);
            if (km) env->SetObjectField(hooker, g.f_keyMatch, km);
        }
        if (sp.url_match) {
            jstring um = env->NewStringUTF(sp.url_match);
            if (um) env->SetObjectField(hooker, g.f_urlMatch, um);
        }
        if (sp.mode == M_REWRITE_BODY) {
            auto set_str = [&](jfieldID f, const std::string& s) {
                if (s.empty()) return;
                jstring js = env->NewStringUTF(s.c_str());
                if (js) env->SetObjectField(hooker, f, js);
            };
            set_str(g.f_openudid, g.vals.openudid);
            set_str(g.f_clientudid, g.vals.clientudid);
            set_str(g.f_cdid, g.vals.cdid);
        }
        if (sp.mode == M_VALUE) {
            if (sp.ret_type == 1) {                     // byte[] target (Widevine)
                if (g.ids.widevine_hex.size() >= 2) {
                    jbyteArray b = sbx_hex_to_jbytes(env, g.ids.widevine_hex);
                    if (b) env->SetObjectField(hooker, g.f_bval, b);
                }
            } else {
                std::string sval = sbx_value_for(sp.val_id, g.vals, g.ids, g.wifi, g.bt);
                if (!sval.empty()) {
                    jstring sv = env->NewStringUTF(sval.c_str());
                    if (sv) env->SetObjectField(hooker, g.f_sval, sv);
                }
            }
        }

        jobject backup = lsplant::Hook(env, target, hooker, g.cb_reflected);
        if (!backup || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        env->SetObjectField(hooker, g.f_backup, backup);

        g.keep.push_back(env->NewGlobalRef(hooker));
        g.keep.push_back(env->NewGlobalRef(backup));

        // Best-effort: defeats inlined callers. LSPlant marks it [[nodiscard]];
        // a failure here costs only the inline-call edge case, not the hook.
        (void)lsplant::Deoptimize(env, target);
        if (env->ExceptionCheck()) env->ExceptionClear();
        return true;
    }();
    env->PopLocalFrame(nullptr);
    if (ok) SBX_LSP_LOGD("L3 hooked %s.%s%s", sp.cls, sp.name, sp.sig);
    return ok;
}

inline bool hook_one(JNIEnv* env, const HookSpec& sp) {
    if (env->PushLocalFrame(8) != 0) { env->ExceptionClear(); return false; }
    jclass cls = env->FindClass(sp.cls);
    bool found = cls && !env->ExceptionCheck();
    if (!found) env->ExceptionClear();
    bool ok = found ? hook_one_class(env, cls, sp) : false;
    env->PopLocalFrame(nullptr);
    return ok;
}

// ---- framework targets: resolvable in any app process at postAppSpecialize ----
inline const HookSpec* hook_specs(size_t& n) {
    static const HookSpec S[] = {
        // ---- P0: the classic device identifiers ----
        { "android/provider/Settings$Secure", "getString",
          "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",
          true, M_VALUE, 1, "android_id", false, 0, V_ANDROID_ID, -1, -1, -1, nullptr },
        { "android/os/Build", "getSerial", "()Ljava/lang/String;",
          true, M_VALUE, -1, nullptr, false, 0, V_SERIAL, -1, -1, -1, nullptr },
        { "android/media/MediaDrm", "getPropertyByteArray", "(Ljava/lang/String;)[B",
          false, M_VALUE, 1, "deviceUniqueId", false, 1, V_WIDEVINE, -1, -1, -1, nullptr },

        // ---- P1: TelephonyManager (all instance methods) ----
        { "android/telephony/TelephonyManager", "getDeviceId", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_IMEI, -1, -1, -1, nullptr },
        { "android/telephony/TelephonyManager", "getDeviceId", "(I)Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_IMEI, -1, -1, -1, nullptr },
        { "android/telephony/TelephonyManager", "getImei", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_IMEI, -1, -1, -1, nullptr },
        { "android/telephony/TelephonyManager", "getImei", "(I)Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_IMEI, -1, -1, -1, nullptr },
        { "android/telephony/TelephonyManager", "getMeid", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_MEID, -1, -1, -1, nullptr },
        { "android/telephony/TelephonyManager", "getMeid", "(I)Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_MEID, -1, -1, -1, nullptr },
        { "android/telephony/TelephonyManager", "getSubscriberId", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_IMSI, -1, -1, -1, nullptr },
        { "android/telephony/TelephonyManager", "getSimSerialNumber", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_ICCID, -1, -1, -1, nullptr },
        { "android/telephony/TelephonyManager", "getNetworkOperator", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_OP_NUM, -1, -1, -1, nullptr },
        { "android/telephony/TelephonyManager", "getSimOperator", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_OP_NUM, -1, -1, -1, nullptr },
        { "android/telephony/TelephonyManager", "getNetworkOperatorName", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_OP_ALPHA, -1, -1, -1, nullptr },
        { "android/telephony/TelephonyManager", "getSimOperatorName", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_OP_ALPHA, -1, -1, -1, nullptr },
        { "android/telephony/TelephonyManager", "getSimCountryIso", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_OP_ISO, -1, -1, -1, nullptr },
        { "android/telephony/TelephonyManager", "getNetworkCountryIso", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_OP_ISO, -1, -1, -1, nullptr },

        // ---- P1: MAC addresses ----
        { "android/net/wifi/WifiInfo", "getMacAddress", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_WIFI_MAC, -1, -1, -1, nullptr },
        { "android/bluetooth/BluetoothAdapter", "getAddress", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_BT_ADDR, -1, -1, -1, nullptr },

        // ---- P2: advertising id (Play services) ----
        { "com/google/android/gms/ads/identifier/AdvertisingIdClient$Info", "getId",
          "()Ljava/lang/String;", false, M_VALUE, -1, nullptr, false, 0, V_GAID,
          -1, -1, -1, nullptr },

        // ---- P3: class-load observer (feeds the TikTok table below) ----
        { "java/lang/ClassLoader", "loadClass", "(Ljava/lang/String;Z)Ljava/lang/Class;",
          false, M_OBSERVE, 1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
    };
    n = sizeof(S) / sizeof(S[0]);
    return S;
}

// ---- TikTok register path -------------------------------------------------
//
// Obfuscated names are per-release. Everything below is resolved by class name
// plus method name/arity at the moment the class is loaded; a release that
// renames a class simply misses that one hook and logs it, instead of failing
// the whole install. The verified map for 47.1.4 is in
// AI-LOCAL/RE-NOTES-TT-47.1.4-HOOKS.md §1-§6.
//
// `cls` is matched as a suffix of the loaded class name, so both "X.088K" and
// "com.bytedance.xxx" style names work.
inline const HookSpec* tt_specs(size_t& n) {
    static const HookSpec S[] = {
        // DeviceParams: never report a device id, so the server mints one.
        { "AbsDeviceParamsProvider", "getDeviceId", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_ZERO, -1, -1, -1, nullptr },
        // ... and report the persona's openudid, which is what the server binds.
        { "AbsDeviceParamsProvider", "getOpenUdid", "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_OPENUDID, -1, -1, -1, nullptr },

        // Write-back merge (X.088K;->LIZIZ(String key, String value)). The
        // register response is merged into the device-id store here, and a
        // merged did outranks every read-path override for the rest of the
        // process. Returning "0" for an all-digit key keeps the write-back from
        // undoing the two hooks above, while non-identity keys pass through.
        //
        // keyArgIndex is 1, not 0: LIZIZ is an instance method and LSPlant's
        // args[0] is the receiver. Testing index 0 would stringify the object,
        // never match an all-digit key, and leave the write-back untouched —
        // exactly the failure the LSPosed variant's Xposed args[0] did not have.
        { "X.088K", "LIZIZ", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
          false, M_ZERO_NUMERIC, 1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },

        // Register replay gate: LX/088c;->LJII(LX/085A;ZZ)V -> force p2 = false
        // so the client performs a real round trip instead of replaying the
        // cached response bound to the old did. args[0]=this, [1]=params,
        // [2]=boolean.
        { "X.088c", "LJII", "(LX/085A;ZZ)V",
          false, M_FORCE_FALSE, -1, nullptr, false, 0, V_NONE, 2, -1, -1, nullptr },

        // Wire rewrite: post(String url, Map, byte[] body).
        //
        // The return type is String, not void — verified against the 47.1.4
        // smali (X/087U.smali:15112, X/085T.smali:8441). Declaring it void
        // makes GetMethodID fail and the hook silently never installs, which
        // would leave the register body unrewritten with only a debug line to
        // show for it.
        //
        // Arg indices are LSPlant's, i.e. they include the receiver: [0]=this,
        // [1]=url, [2]=headers Map, [3]=body byte[].
        { "X.087U", "post", "(Ljava/lang/String;Ljava/util/Map;[B)Ljava/lang/String;",
          false, M_REWRITE_BODY, -1, nullptr, false, 0, V_NONE, -1, 3, 1,
          "/device_register/" },
        { "X.085T", "post", "(Ljava/lang/String;Ljava/util/Map;[B)Ljava/lang/String;",
          false, M_REWRITE_BODY, -1, nullptr, false, 0, V_NONE, -1, 3, 1,
          "/device_register/" },

        // Keva: the SDK's own key/value cache, which is where the persisted
        // openudid/clientudid/cdid are read back from on a warm start.
        // getString is an INSTANCE method, so keyArgIndex is 1 (args[0] is the
        // adapter itself). Index 0 would test the receiver, never match, and
        // leave the stored value in place.
        { "com.bytedance.keva.adapter.KevaSpFastAdapter", "getString",
          "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
          false, M_VALUE, 1, "openudid", false, 0, V_OPENUDID, -1, -1, -1, nullptr },
        { "com.bytedance.keva.adapter.KevaSpFastAdapter", "getString",
          "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
          false, M_VALUE, 1, "clientudid", false, 0, V_CLIENTUDID, -1, -1, -1, nullptr },
        { "com.bytedance.keva.adapter.KevaSpFastAdapter", "getString",
          "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
          false, M_VALUE, 1, "cdid", false, 0, V_CDID, -1, -1, -1, nullptr },
        // The obfuscated sibling adapter. The LSPosed variant hooks both and
        // records that patching only one left identity fields leaking, so the
        // same two are hooked here. X.02tJ renames per release: a miss costs
        // this one adapter and is logged, not fatal.
        { "X.02tJ", "getString",
          "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
          false, M_VALUE, 1, "openudid", false, 0, V_OPENUDID, -1, -1, -1, nullptr },
        { "X.02tJ", "getString",
          "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
          false, M_VALUE, 1, "clientudid", false, 0, V_CLIENTUDID, -1, -1, -1, nullptr },
        { "X.02tJ", "getString",
          "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
          false, M_VALUE, 1, "cdid", false, 0, V_CDID, -1, -1, -1, nullptr },
    };
    n = sizeof(S) / sizeof(S[0]);
    return S;
}

// The class-load observer. Java filters on the watch list before calling here,
// so this only runs for a class we actually have specs for.
inline void sbx_observe(JNIEnv* env, jclass, jstring name, jclass cls) {
    if (!env || !cls || !name) return;

    // Re-entrancy guard: resolving a method here can itself trigger class
    // loading (parameter types, exceptions), which would call this hook again
    // and recurse without bound.
    static thread_local bool in_observe = false;
    if (in_observe) return;
    in_observe = true;

    const char* raw = env->GetStringUTFChars(name, nullptr);
    if (raw) {
        std::string cn(raw);
        env->ReleaseStringUTFChars(name, raw);

        size_t n = 0;
        const HookSpec* specs = tt_specs(n);
        int good = 0;
        for (size_t i = 0; i < n; ++i) {
            const std::string suffix(specs[i].cls);
            if (cn.size() < suffix.size()) continue;
            if (cn.compare(cn.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            if (hook_one_class(env, cls, specs[i])) ++good;
        }
        if (good) SBX_LSP_LOGD("L3: %d TikTok target(s) hooked on %s", good, cn.c_str());
    } else {
        env->ExceptionClear();
    }
    in_observe = false;
}

inline bool install_all(JNIEnv* env, const HookValues& v) {
    if (!env) return false;
    if (!init(env)) return false;

    g.vals = v;
    if (!g.cb_class) {
        g.cb_class = load_callback_class(env);
        if (!g.cb_class || !resolve_callback_members(env)) {
            SBX_LSP_LOGE("L3: callback class/members unavailable - hooks skipped");
            return false;
        }
    }

    // Telephony/DRM identifiers, synthesized from the persona seed for cross-field
    // consistency and rotated with every fresh identity.
    g.ids  = sbxid::synth_all(v.seed, v.op_num);
    g.wifi = !v.wifi_mac.empty() ? v.wifi_mac
                                 : sbxnr::mac_from_seed(v.seed ^ 0x5749464931ULL);
    g.bt   = sbx_mac_upper(sbxnr::mac_from_seed(v.seed ^ 0x424C554554ULL));

    size_t n = 0;
    const HookSpec* specs = hook_specs(n);
    int good = 0;
    for (size_t i = 0; i < n; ++i)
        if (hook_one(env, specs[i])) ++good;

    SBX_LSP_LOGD("L3 install_all: %d/%zu targets hooked", good, n);
    if (good == 0) SBX_LSP_LOGE("L3: no targets hooked (continuing with L1/L2/L9)");
    return good > 0;
}
#endif // SBX_ENABLE_LSPLANT

} // namespace sbxlsp
