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
#include <set>
#include <mutex>
#include <cstdint>
#include <atomic>

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
    // WebView User-Agent rewrite, as one literal {from, to} pair. `ua_from` is
    // the real "Android <release>; <model> Build/<id>" segment this device
    // actually produces; `ua_to` is the persona's. Both are built at install
    // time from the real props (reading them is not a leak - the module is the
    // one reading) so the rewrite follows whatever persona is loaded and stays
    // valid across WebView updates, which change the rest of the UA string.
    std::string ua_from;
    std::string ua_to;
    // OEM detection classes to make Class.forName reject. Built from the
    // persona's manufacturer: a Pixel persona must not be able to load
    // miui.os.Build, because an OEM branch that loads is an OEM branch that
    // reports. Empty on a persona that IS that OEM.
    std::vector<std::string> deny_classes;
    uint64_t    seed = 0;   // fnv1a(FINGERPRINT|SERIAL|ANDROID_ID)
    int         display_width   = 0;
    int         display_height  = 0;
    int         display_density = 0;
    float       display_xdpi    = 0.0f;
    float       display_ydpi    = 0.0f;
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

// Resolved in init(); the address LSPlant passes as the target of its
// Class::SetStatus hook. See the refusal in sbx_inline_hooker below.
inline void* g_set_status = nullptr;

// True when `p` starts with Dobby's arm64 absolute-branch veneer:
//     ldr xN, #8      (0x58000000 | imm19=2 | N)
//     br  xN          (0xD61F0000 | N<<5)
//     .quad target
// arm64 only: the other ABIs encode their veneers differently, and matching
// these words against ARM32/x86 code would be meaningless.
inline bool sbx_is_veneer(const void* p) {
#if defined(__aarch64__)
    const uint32_t* w = static_cast<const uint32_t*>(p);
    // ldr (literal): opc=01 in bits 31:30, V=0 (bit 26), rt in bits 4:0.
    if ((w[0] & 0xFF000000u) != 0x58000000u) return false;
    if (((w[0] >> 5) & 0x7FFFFu) != 2u) return false;        // ... at pc+8
    const uint32_t n = w[0] & 0x1Fu;
    // br <xN>: everything except Rn (bits 9:5) must match the br base.
    return (w[1] & 0xFFFFFC00u) == 0xD61F0000u &&
           ((w[1] >> 5) & 0x1Fu) == n;
#else
    (void)p;
    return false;
#endif
}

inline void* sbx_inline_hooker(void* target, void* hooker) {
    // LSPlant installs an inline hook on art::mirror::Class::SetStatus so it can
    // back up the entry point of a static method when its class is initialized.
    // On this ROM that hook leaves Class::replace branching to a null backup.
    // replace ends with `br x3` (a tail branch, so no frame is pushed) and x3
    // comes from the hooker's backup slot; when that slot is null the process
    // spins at 100% CPU with a one-frame stack until ActivityManager kills it
    // ("am_kill ... start timeout"). That is what made every target app fail to
    // launch on v2.2.22 — the first version whose symbol resolver was good
    // enough for lsplant::Init to succeed, and load_callback_class() triggers
    // the first DefineClass, hence the first SetStatus call.
    //
    // Refusing this one hook is safe. HookSymNoHandle() treats any non-null
    // target as success regardless of what the hooker returned, so LSPlant still
    // initialises and all the other hooks install; this entry is simply never
    // patched, so the bad backup cannot form. The cost is LSPlant's late-init
    // backup: a static method hooked *before* its class is initialized can lose
    // its entry point when that class is initialized later. Every framework
    // class this module targets is already initialized long before
    // postAppSpecialize, so the cost is narrow — and strictly better than an app
    // that cannot start.
    if (target && target == g_set_status) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            SBX_LSP_LOGE("L3: skipping the Class::SetStatus inline hook - on "
                         "this ROM its backup slot stays null and Class::replace "
                         "tail-branches into it. Other hooks install normally.");
        }
        return nullptr;
    }

    // Already carrying someone else's veneer: refuse.
    //
    // On this ROM art::ClassLinker::RegisterNative (and UnregisterNative) start
    // with `nop; sub sp,#N`, but another module in this process has hooked them
    // first, so the entry now reads `ldr x17,#8; br x17; .quad <their replace>`.
    // DobbyHook does not notice — it finds no entry of its own and proceeds. It
    // sizes the relocation block from the *trampoline buffer* (16 bytes), copies
    // those 16 bytes as "the original prologue" into the new trampoline, and
    // appends LiteralLdrBranch(entry + 16) — which lands in the middle of our
    // new veneer's literal. Calling that trampoline then reloads the literal
    // (now holding our replace), and replace ends in `br x4` (a tail branch, so
    // no frame is pushed) back into the trampoline: replace -> backup -> replace
    // for ever at 100% CPU, one frame deep, until "am_kill ... start timeout".
    // That is what kept every target app from launching on v2.2.23, right after
    // the Class::SetStatus refusal let lsplant::Init succeed for the first time
    // and these RegisterNative hooks became reachable.
    //
    // Refusing is safe for the same reason as the SetStatus refusal above: the
    // caller treats any non-null target as success, so the rest of LSPlant still
    // installs. LSPlant only wants these hooks to propagate a backup entry point
    // when ART re-registers a native method; leaving the other module's hook in
    // place keeps ART working and simply skips that propagation for it.
    if (sbx_is_veneer(target)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            SBX_LSP_LOGE("L3: %p is already inline-hooked by another module - "
                         "leaving it alone. Dobby would otherwise relocate the "
                         "foreign veneer and build a trampoline that loops back "
                         "into our replace.", target);
        }
        return nullptr;
    }

    void* origin = nullptr;
    // dobby_dummy_func_t is `void (*)()` — an untyped slot, the same convention
    // upstream uses for DobbyHook's replace/origin pair.
    if (DobbyHook(target, reinterpret_cast<dobby_dummy_func_t>(hooker),
                  reinterpret_cast<dobby_dummy_func_t*>(&origin)) != 0) return nullptr;
    // Dobby returns RS_SUCCESS even when GenerateRelocatedCode() bailed out; in
    // that case origin is null, the entry is already patched, and LSPlant would
    // take the null as a valid backup and branch into it. Ask Dobby to undo the
    // patch and report failure. The undo is best-effort: on the relocate-failed
    // path origin_insn_size is 0, so the patch may survive — which is why the
    // SetStatus refusal above, not this branch, is what keeps the process alive.
    if (!origin) {
        DobbyDestroy(target);
        SBX_LSP_LOGE("L3: DobbyHook reported success but produced no trampoline "
                     "for %p - hook refused", target);
        return nullptr;
    }
    return origin;
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
    M_REWRITE_STRING = 5,// call the original, then replace device tells in its String
    M_DISPLAY_METRICS = 6,// spoof DisplayMetrics, Configuration, Point, Rect, Display dimensions
    M_APP_ATTACH      = 7,// intercept Application attach/create and hook app-level targets
    M_FILTER_PACKAGES = 8,// hide root apps & tools from PackageManager queries
    M_VALUE_MULTI     = 9,// return the spoof value for the gated key, from a key->value table
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
    // M_VALUE_MULTI: parallel key/value tables, multi_count entries each.
    // Declared last so every existing positional initialiser in the spec
    // tables stays valid (the extra members default to null/0).
    const char* const* multi_keys = nullptr;
    const int*         multi_vals = nullptr;
    int                multi_count = 0;
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
              f_cdid = nullptr, f_rwFrom = nullptr, f_rwTo = nullptr,
              f_deny = nullptr,
              f_multiKeys = nullptr, f_multiVals = nullptr,
              f_dispW = nullptr, f_dispH = nullptr, f_dispDpi = nullptr,
              f_dispXdpi = nullptr, f_dispYdpi = nullptr,
              f_dispMetricType = nullptr;
    // The app's own ClassLoader, learned at Application.attachBaseContext (or
    // from the first observed class load). Needed because FindClass can only
    // ever see the boot class path - see sbx_find_class().
    jobject   app_loader = nullptr;   // global ref, owned here
    std::vector<jobject> keep;   // global refs: GC anchor for hookers + backups
    HookValues vals;
    sbxid::SynthIds ids;
    std::string wifi, bt;
};
inline State g;

// Set while a class resolution is in flight. Resolving a class through the
// app's ClassLoader runs the app's own loadClass, which is one of our hooked
// methods, so without this the observer would re-enter and try to hook the very
// spec the resolution is already installing.
inline thread_local bool g_in_resolve = false;

// Framework specs that failed their first install, kept so they can be retried
// once the app's ClassLoader is known. Only the failures are kept: re-running
// an already-installed spec would be refused by LSPlant (its backup would be
// the previous hooker) and logged as a miss, which is exactly the noise this
// list exists to avoid.
inline std::vector<const HookSpec*>& sbx_pending() {
    static std::vector<const HookSpec*> v;
    return v;
}

// Specs already installed in this process, keyed "cls|name|sig".
//
// Two independent paths can install the same tt_specs entry: the class-load
// observer (a class the app asks for by name) and the app-attach loop (a class
// the loop asks for itself). Both can reach the same class - the loop's own
// loadClass goes through the observer unless it is suppressed - and LSPlant
// refuses the second hook on an already-hooked ArtMethod, so without this the
// loser of the race logged "L3 MISS ... lsplant::Hook refused" for a method
// that was in fact hooked. That is worse than noise: it is a false negative in
// the one log line meant to name real misses.
//
// Guarded by a mutex because loadClass can run on any thread.
inline std::mutex& sbx_hooked_mu() {
    static std::mutex m;
    return m;
}
inline std::set<std::string>& sbx_hooked_set() {
    static std::set<std::string> s;
    return s;
}

// Returns false if this spec is already installed, so the caller skips it
// instead of provoking an LSPlant refusal.
inline bool sbx_claim_spec(const HookSpec& sp) {
    std::string key(sp.cls);
    key += '|'; key += sp.name;
    key += '|'; key += sp.sig;
    std::lock_guard<std::mutex> lk(sbx_hooked_mu());
    return sbx_hooked_set().insert(std::move(key)).second;
}

// Give the claim back when the install failed, so a later retry can try again.
inline void sbx_release_spec(const HookSpec& sp) {
    std::string key(sp.cls);
    key += '|'; key += sp.name;
    key += '|'; key += sp.sig;
    std::lock_guard<std::mutex> lk(sbx_hooked_mu());
    sbx_hooked_set().erase(key);
}

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
inline bool sbx_on_app_created(JNIEnv* env, jobject clLoader);
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
    g.f_rwFrom     = env->GetFieldID(g.cb_class, "rwFrom",       "[Ljava/lang/String;");
    g.f_rwTo       = env->GetFieldID(g.cb_class, "rwTo",         "[Ljava/lang/String;");
    g.f_multiKeys  = env->GetFieldID(g.cb_class, "multiKeys",    "[Ljava/lang/String;");
    g.f_multiVals  = env->GetFieldID(g.cb_class, "multiVals",    "[Ljava/lang/String;");
    g.f_deny       = env->GetStaticFieldID(g.cb_class, "deny",    "[Ljava/lang/String;");

    g.f_dispW          = env->GetFieldID(g.cb_class, "displayWidth",      "I");
    g.f_dispH          = env->GetFieldID(g.cb_class, "displayHeight",     "I");
    g.f_dispDpi        = env->GetFieldID(g.cb_class, "displayDensity",    "I");
    g.f_dispXdpi       = env->GetFieldID(g.cb_class, "displayXdpi",       "F");
    g.f_dispYdpi       = env->GetFieldID(g.cb_class, "displayYdpi",       "F");
    g.f_dispMetricType = env->GetFieldID(g.cb_class, "displayMetricType", "I");

    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }

    static const JNINativeMethod NATIVE[] = {
        { (char*)"observe", (char*)"(Ljava/lang/String;Ljava/lang/Class;)V",
          (void*)+[](JNIEnv* e, jclass thiz, jstring n, jclass c) {
              sbx_observe(e, thiz, n, c);
          } },
        { (char*)"onAppCreated", (char*)"(Ljava/lang/ClassLoader;)Z",
          (void*)+[](JNIEnv* e, jclass, jobject cl) -> jboolean {
              return (jboolean)(sbx_on_app_created(e, cl) ? JNI_TRUE : JNI_FALSE);
          } },
    };
    if (env->RegisterNatives(g.cb_class, NATIVE, 2) != 0) {
        env->ExceptionClear();
        SBX_LSP_LOGD("L3: RegisterNatives(observe, onAppCreated) gagal - jalur TikTok/app nonaktif");
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

    // Denied OEM classes: names the app must not be able to load, because an
    // AOSP device does not ship them. Static, like `watch`: the forName hook is
    // shared by every caller and the list is a property of the persona, not of
    // one hooked method. install_all() fills it from HookValues once the class
    // is available (resolve_callback_members runs before that).
    return g.f_mode && g.f_isStatic && g.f_keyIdx && g.f_keyMatch && g.f_keyContains &&
           g.f_retType && g.f_sval && g.f_bval && g.f_backup && g.f_forceIdx &&
           g.f_bodyIdx && g.f_urlIdx && g.f_urlMatch && g.f_openudid &&
           g.f_clientudid && g.f_cdid && g.f_rwFrom && g.f_rwTo && g.f_deny &&
           g.f_multiKeys && g.f_multiVals &&
           g.f_dispW && g.f_dispH && g.f_dispDpi && g.f_dispMetricType;
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

    // The exact name LSPlant resolves for its Class::SetStatus hook (see
    // sbx_inline_hooker). Resolved through the same resolver LSPlant uses, so
    // the comparison there is against the identical pointer.
    g_set_status = art.getSymbAddressOrUniq(
        "_ZN3art6mirror5Class9SetStatusENS_6HandleIS1_EENS_11ClassStatusEPNS_6ThreadE");

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

// Resolve a spec's class, preferring the app's own ClassLoader.
//
// FindClass alone is not enough, and the failure is structural rather than a
// naming mistake. FindClass resolves against the *boot* class path (the JNI
// spec pins it to the caller's loader, and a Zygisk companion's caller is the
// system one), so it can only ever see android.*, java.* and anything on
// BOOTCLASSPATH. A class that lives in the app's APK is invisible to it even
// though the app itself is already using the class - and the class-load
// observer cannot cover for it either, because a class reached through a *type
// reference* (a parameter or return type) is resolved by the runtime's own
// linker, never through ClassLoader.loadClass(String), so the Java hook is
// never called for it.
//
// The one spec that hits this is
// com/google/android/gms/ads/identifier/AdvertisingIdClient$Info: both TikTok
// builds BUNDLE the Play-services advertising-id stub in their own classes*.dex
// (classes6.dex in trill 47.0.3 and musically 47.1.4), and reference it as a
// return type from AdvertisingIdClient.getAdvertisingIdInfo(Context) in 17
// places. It therefore never reached the observer, FindClass never saw it, and
// the spec silently failed in every session - the "41/42" count with no line
// naming the miss.
//
// Order matters: FindClass first (cheap, and correct for every framework spec),
// then the app loader. The loader is only known once the app has attached, so
// before that this degrades exactly to the old behaviour.
inline jclass sbx_find_class(JNIEnv* env, const char* cls) {
    jclass c = env->FindClass(cls);
    if (c && !env->ExceptionCheck()) return c;
    env->ExceptionClear();

    if (!g.app_loader) return nullptr;
    if (g_in_resolve) return nullptr;          // re-entrancy: see g_in_resolve

    // JNI wants a dotted binary name; the table stores slashed descriptors.
    std::string dot(cls);
    for (char& ch : dot) { if (ch == '/') ch = '.'; }

    jclass clCls = env->FindClass("java/lang/ClassLoader");
    if (!clCls || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    jmethodID loadClass = env->GetMethodID(clCls, "loadClass",
                                           "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!loadClass || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

    jstring name = env->NewStringUTF(dot.c_str());
    if (!name) return nullptr;
    g_in_resolve = true;
    jobject o = env->CallObjectMethod(g.app_loader, loadClass, name);
    g_in_resolve = false;
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return (jclass)o;
}

// Hook one spec against an already-resolved class. Used both for the framework
// targets (FindClass by name) and for the TikTok targets (handed over by the
// class-load observer). Fail-soft: any miss returns false and is skipped.
inline bool hook_one_class(JNIEnv* env, jclass cls, const HookSpec& sp) {
    if (!cls) return false;
    // Already installed by the other path (observer vs attach loop). Skipping
    // silently is correct: the method IS hooked, and reporting a miss here
    // would be the false negative this guard exists to prevent.
    if (!sbx_claim_spec(sp)) return true;
    if (env->PushLocalFrame(24) != 0) {
        env->ExceptionClear();
        sbx_release_spec(sp);
        return false;
    }
    // Why a spec did not install. A silent miss is indistinguishable from a
    // spec that was never in the table: the installed TikTok build drifted
    // (classes present, signatures changed) and GAID never resolved, and both
    // only showed up as the "41/42" count with no way to tell which spec and
    // which of the six failure points it was. The reason is logged at DEBUG so
    // a normal session stays quiet while a debug session names every miss.
    const char* why = nullptr;
    bool ok = [&]() -> bool {
        jmethodID mid = sp.is_static ? env->GetStaticMethodID(cls, sp.name, sp.sig)
                                     : env->GetMethodID(cls, sp.name, sp.sig);
        if (!mid || env->ExceptionCheck()) { env->ExceptionClear(); why = "method not found (signature drift?)"; return false; }
        jobject target = env->ToReflectedMethod(cls, mid, sp.is_static ? JNI_TRUE : JNI_FALSE);
        if (!target || env->ExceptionCheck()) { env->ExceptionClear(); why = "ToReflectedMethod failed"; return false; }

        jobject hooker = env->NewObject(g.cb_class, g.cb_ctor);
        if (!hooker || env->ExceptionCheck()) { env->ExceptionClear(); why = "callback ctor failed"; return false; }
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
        if (sp.mode == M_REWRITE_STRING) {
            // One pair, both halves required: a from-without-to (or the
            // reverse) would make the Java side skip the substitution, which is
            // the right outcome but only after a wasted hook.
            if (!g.vals.ua_from.empty() && !g.vals.ua_to.empty()) {
                jclass strCls = env->FindClass("java/lang/String");
                if (strCls) {
                    auto arr1 = [&](const std::string& s) -> jobjectArray {
                        jobjectArray a = env->NewObjectArray(1, strCls, nullptr);
                        if (!a) return nullptr;
                        jstring js = env->NewStringUTF(s.c_str());
                        if (js) env->SetObjectArrayElement(a, 0, js);
                        return a;
                    };
                    jobjectArray from = arr1(g.vals.ua_from);
                    jobjectArray to   = arr1(g.vals.ua_to);
                    if (from) env->SetObjectField(hooker, g.f_rwFrom, from);
                    if (to)   env->SetObjectField(hooker, g.f_rwTo, to);
                }
            }
        }
        if (sp.mode == M_DISPLAY_METRICS) {
            if (g.f_dispW)          env->SetIntField(hooker, g.f_dispW, g.vals.display_width);
            if (g.f_dispH)          env->SetIntField(hooker, g.f_dispH, g.vals.display_height);
            if (g.f_dispDpi)        env->SetIntField(hooker, g.f_dispDpi, g.vals.display_density);
            if (g.f_dispXdpi)       env->SetFloatField(hooker, g.f_dispXdpi, g.vals.display_xdpi);
            if (g.f_dispYdpi)       env->SetFloatField(hooker, g.f_dispYdpi, g.vals.display_ydpi);
            if (g.f_dispMetricType) env->SetIntField(hooker, g.f_dispMetricType, sp.key_index);
        }
        if (sp.mode == M_VALUE_MULTI) {
            // One hook answering several keys of the same method. The keys and
            // their spoofed values are built from the spec's multi list, and
            // the values come from the same sbx_value_for() the single-key path
            // uses, so the two cannot drift apart.
            jclass strCls = env->FindClass("java/lang/String");
            if (strCls && sp.multi_keys && sp.multi_vals && sp.multi_count > 0) {
                jobjectArray keys = env->NewObjectArray(sp.multi_count, strCls, nullptr);
                jobjectArray vals = env->NewObjectArray(sp.multi_count, strCls, nullptr);
                if (keys && vals) {
                    for (int i = 0; i < sp.multi_count; ++i) {
                        jstring k = env->NewStringUTF(sp.multi_keys[i]);
                        if (k) env->SetObjectArrayElement(keys, i, k);
                        std::string v = sbx_value_for(sp.multi_vals[i], g.vals, g.ids,
                                                      g.wifi, g.bt);
                        if (!v.empty()) {
                            jstring jv = env->NewStringUTF(v.c_str());
                            if (jv) env->SetObjectArrayElement(vals, i, jv);
                        }
                    }
                    env->SetObjectField(hooker, g.f_multiKeys, keys);
                    env->SetObjectField(hooker, g.f_multiVals, vals);
                }
            }
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
        if (!backup || env->ExceptionCheck()) { env->ExceptionClear(); why = "lsplant::Hook refused"; return false; }
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
    if (ok) {
        SBX_LSP_LOGD("L3 hooked %s.%s%s", sp.cls, sp.name, sp.sig);
    } else {
        // Hand the claim back: the method is not hooked, so a later retry (the
        // app-attach pass, once the loader is known) must be allowed to try.
        sbx_release_spec(sp);
        SBX_LSP_LOGD("L3 MISS %s.%s%s - %s", sp.cls, sp.name, sp.sig,
                     why ? why : "unknown");
    }
    return ok;
}

inline bool hook_one(JNIEnv* env, const HookSpec& sp) {
    if (env->PushLocalFrame(8) != 0) { env->ExceptionClear(); return false; }
    jclass cls = sbx_find_class(env, sp.cls);
    bool found = cls && !env->ExceptionCheck();
    if (!found) env->ExceptionClear();
    bool ok = found ? hook_one_class(env, cls, sp) : false;
    if (!found)
        SBX_LSP_LOGD("L3 MISS %s.%s%s - class not found", sp.cls, sp.name, sp.sig);
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

        // ---- P3b: OEM class deny + by-name load observer ----
        //
        // The 3-arg overload, not the 1-arg one: forName(String) is
        // @CallerSensitive and resolves through Reflection.getCallerClass(),
        // which from this hook would see EnvCompatState (loaded by the module's
        // own InMemoryDexClassLoader) instead of the app - every by-name load
        // would resolve against the wrong loader. The 3-arg overload takes the
        // loader as an argument and passes it straight to classForName, so it
        // is the one place a deny or an observation is free of that hazard.
        // key_index 0 = the class name (the method is static).
        { "java/lang/Class", "forName",
          "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;",
          true, M_OBSERVE, 0, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },

        // ---- P4: WebView User-Agent ----
        // libwebviewchromium.so composes the UA from ro.product.model /
        // ro.build.id / ro.build.version.release, and on this ROM that library
        // cannot be PLT-hooked at all: its read-only segments are mapped from
        // /data/misc/shared_relro/libwebviewchromium64.relro (a relocation
        // blob, not an ELF - no header at offset 0) and its executable segment
        // from an uncompressed entry inside the WebView base.apk. Neither
        // mapping starts with an ELF header, so neither LSPlt nor PLTI can
        // derive a base and the GOT redirect never reaches it. The Java surface
        // is therefore the only place the value can be rewritten.
        //
        // WebSettings.getDefaultUserAgent(Context) is the framework entry point
        // the app calls for the "…; wv) … Version/4.0 …" form.
        { "android/webkit/WebSettings", "getDefaultUserAgent",
          "(Landroid/content/Context;)Ljava/lang/String;",
          true, M_REWRITE_STRING, -1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },

        // ---- P5: Display metrics & screen resolution ----
        { "android/content/res/Resources", "getDisplayMetrics",
          "()Landroid/util/DisplayMetrics;",
          false, M_DISPLAY_METRICS, 0, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/view/Display", "getMetrics",
          "(Landroid/util/DisplayMetrics;)V",
          false, M_DISPLAY_METRICS, 0, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/view/Display", "getRealMetrics",
          "(Landroid/util/DisplayMetrics;)V",
          false, M_DISPLAY_METRICS, 0, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/view/Display", "getSize",
          "(Landroid/graphics/Point;)V",
          false, M_DISPLAY_METRICS, 0, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/view/Display", "getRealSize",
          "(Landroid/graphics/Point;)V",
          false, M_DISPLAY_METRICS, 0, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/view/Display", "getWidth",
          "()I",
          false, M_DISPLAY_METRICS, 1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/view/Display", "getHeight",
          "()I",
          false, M_DISPLAY_METRICS, 2, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/view/WindowMetrics", "getBounds",
          "()Landroid/graphics/Rect;",
          false, M_DISPLAY_METRICS, 0, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/content/res/Resources", "getConfiguration",
          "()Landroid/content/res/Configuration;",
          false, M_DISPLAY_METRICS, 0, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },

        // ---- P6: App lifecycle / APK class loader entry point ----
        { "android/app/Application", "attachBaseContext",
          "(Landroid/content/Context;)V",
          false, M_APP_ATTACH, -1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/app/Instrumentation", "callApplicationOnCreate",
          "(Landroid/app/Application;)V",
          false, M_APP_ATTACH, -1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },

        // ---- P7: PackageManager root & mod cloaking ----
        { "android/app/ApplicationPackageManager", "getInstalledPackages",
          "(I)Ljava/util/List;",
          false, M_FILTER_PACKAGES, -1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/app/ApplicationPackageManager", "getInstalledApplications",
          "(I)Ljava/util/List;",
          false, M_FILTER_PACKAGES, -1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/app/ApplicationPackageManager", "getPackageInfo",
          "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",
          false, M_FILTER_PACKAGES, -1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/app/ApplicationPackageManager", "getApplicationInfo",
          "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;",
          false, M_FILTER_PACKAGES, -1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        // Android 13+ (API 33+) PackageInfoFlags / ApplicationInfoFlags overloads
        { "android/app/ApplicationPackageManager", "getInstalledPackages",
          "(Landroid/content/pm/PackageManager$PackageInfoFlags;)Ljava/util/List;",
          false, M_FILTER_PACKAGES, -1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/app/ApplicationPackageManager", "getInstalledApplications",
          "(Landroid/content/pm/PackageManager$ApplicationInfoFlags;)Ljava/util/List;",
          false, M_FILTER_PACKAGES, -1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/app/ApplicationPackageManager", "getPackageInfo",
          "(Ljava/lang/String;Landroid/content/pm/PackageManager$PackageInfoFlags;)Landroid/content/pm/PackageInfo;",
          false, M_FILTER_PACKAGES, -1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "android/app/ApplicationPackageManager", "getApplicationInfo",
          "(Ljava/lang/String;Landroid/content/pm/PackageManager$ApplicationInfoFlags;)Landroid/content/pm/ApplicationInfo;",
          false, M_FILTER_PACKAGES, -1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
    };
    n = sizeof(S) / sizeof(S[0]);
    return S;
}

// ---- TikTok register path -------------------------------------------------
//
// Obfuscated names are per-release. Everything below is resolved by class name
// plus method name/arity at the moment the class is loaded; a release that
// renames a class simply misses that one hook and logs it, instead of failing
// the whole install.
//
// The table was verified against BOTH installed builds by reading their DEX
// directly (jni/tools has no DEX reader; the check was a standalone parser over
// the extracted classes*.dex):
//
//   trill      47.0.3  com.ss.android.ugc.trill
//   musically  47.1.4  com.zhiliaoapp.musically
//
// That check is what found the drift this comment used to get wrong: the names
// below are NOT all shared between the two builds, and the class-load observer
// cannot detect the difference when a class exists under the same name with a
// different method signature. See the per-spec notes.
//
// `cls` is matched as a suffix of the loaded class name, so both "X.088K" and
// "com.bytedance.xxx" style names work.
// The Keva keys whose stored values are AppLog identity, and which of our
// values each one must return. Shared by both Keva adapters in tt_specs().
//
// These are the key names the SDK itself uses; verified present in the
// installed trill 47.0.3 DEX string tables (openudid: 6 DEX, clientudid: 4,
// cdid: 6). "bd_openudid" and "client_udid" do NOT exist in this build and
// were deliberately not added.
static const char* const kKevaKeys[] = { "openudid", "clientudid", "cdid" };
static const int kKevaVals[] = { V_OPENUDID, V_CLIENTUDID, V_CDID };
static const int kKevaKeyCount = 3;

inline const HookSpec* tt_specs(size_t& n) {
    static const HookSpec S[] = {
        // AppLog public SDK endpoints: spoof client IDs (openudid, clientudid).
        // NOTE: getDid() and getIid() must NOT be hooked to V_ZERO here.
        // Returning "0" causes application modules (including TikTok Shop / Lynx
        // checkout and PIPO security SDK) to send device_id="0" on order submission,
        // which triggers ecom_order_create_account_risk. Device registration
        // zero-did is handled by AbsDeviceParamsProvider.getDeviceId() below.
        { "com.bytedance.applog.AppLog", "getOpenUdid", "()Ljava/lang/String;",
          true, M_VALUE, -1, nullptr, false, 0, V_OPENUDID, -1, -1, -1, nullptr },
        { "com.bytedance.applog.AppLog", "getClientUdid", "()Ljava/lang/String;",
          true, M_VALUE, -1, nullptr, false, 0, V_CLIENTUDID, -1, -1, -1, nullptr },

        // DeviceRegisterManager public SDK endpoints: spoof client IDs.
        // getDeviceId* and getInstallId* must return the server-minted IDs.
        { "com.ss.android.deviceregister.DeviceRegisterManager", "getOpenUdId", "()Ljava/lang/String;",
          true, M_VALUE, -1, nullptr, false, 0, V_OPENUDID, -1, -1, -1, nullptr },
        { "com.ss.android.deviceregister.DeviceRegisterManager", "getClientUDID", "()Ljava/lang/String;",
          true, M_VALUE, -1, nullptr, false, 0, V_CLIENTUDID, -1, -1, -1, nullptr },
        { "com.ss.android.deviceregister.DeviceRegisterManager", "getClientUDIDWithBackup", "()Ljava/lang/String;",
          true, M_VALUE, -1, nullptr, false, 0, V_CLIENTUDID, -1, -1, -1, nullptr },
        { "com.ss.android.deviceregister.DeviceRegisterManager", "getCdid", "(Landroid/content/Context;)Ljava/lang/String;",
          true, M_VALUE, -1, nullptr, false, 0, V_CDID, -1, -1, -1, nullptr },

        // DeviceParams: never report a device id, so the server mints one.
        //
        // The FULL class name, not the bare "AbsDeviceParamsProvider" the spec
        // used to carry. A name with no dot or slash cannot be resolved by
        // sbx_on_app_created (it skips those, having nothing to pass to
        // ClassLoader.loadClass), so the only way this spec could ever install
        // was the class-load observer seeing the app ask for that exact name -
        // and the app never does. Verified on device: no session contains a
        // single line mentioning bdinstall or AbsDeviceParams, and the
        // onAppCreated path never ran at all. The class exists in both builds
        // at Lcom/bytedance/bdinstall/storage/AbsDeviceParamsProvider; with
        // getDeviceId()String and getOpenUdid()String, so naming it in full
        // makes it resolvable.
        //
        // The suffix match in sbx_observe still accepts the short form, so
        // this is strictly more reachable than before, not differently matched.
        { "com.bytedance.bdinstall.storage.AbsDeviceParamsProvider", "getDeviceId",
          "()Ljava/lang/String;",
          false, M_VALUE, -1, nullptr, false, 0, V_ZERO, -1, -1, -1, nullptr },
        // ... and report the persona's openudid, which is what the server binds.
        { "com.bytedance.bdinstall.storage.AbsDeviceParamsProvider", "getOpenUdid",
          "()Ljava/lang/String;",
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
        //
        // The class name is per-build. Both were confirmed by DEX read, and the
        // pairing is not a guess: the two classes have byte-identical method
        // sets, differing only in the names of the types they mention.
        //
        //   musically 47.1.4 : LX/088K;  LIZIZ(String,String)String
        //   trill     47.0.3 : LX/07Wv;  LIZIZ(String,String)String
        //
        //   musically: <init>()V LIZ(String)String LIZIZ(String,String)String
        //              LIZJ(String,String)V LIZLLL(List)V
        //   trill    : <init>()V LIZ(String)String LIZIZ(String,String)String
        //              LIZJ(String,String)V LIZLLL(List)V
        { "X.088K", "LIZIZ", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
          false, M_ZERO_NUMERIC, 1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "X.07Wv", "LIZIZ", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
          false, M_ZERO_NUMERIC, 1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },

        // Register replay gate: LJII(LX/085A;ZZ)V -> force p2 = false
        // so the client performs a real round trip instead of replaying the
        // cached response bound to the old did. args[0]=this, [1]=params,
        // [2]=boolean.
        //
        // Same per-build rename. The pairing is again structural: both classes
        // carry the same 12 methods, including the LJII(L*;ZZ)V +
        // LIZIZ(L*;Z)Z pair that no other class in either build has.
        //
        //   musically 47.1.4 : LX/088c;  LJII(LX/085A;ZZ)V
        //   trill     47.0.3 : LX/07XI;  LJII(LX/07TX;ZZ)V
        { "X.088c", "LJII", "(LX/085A;ZZ)V",
          false, M_FORCE_FALSE, -1, nullptr, false, 0, V_NONE, 2, -1, -1, nullptr },
        { "X.07XI", "LJII", "(LX/07TX;ZZ)V",
          false, M_FORCE_FALSE, -1, nullptr, false, 0, V_NONE, 2, -1, -1, nullptr },

        // Wire rewrite: post(String url, Map, byte[] body).
        //
        // The return type is String, not void. Declaring it void makes
        // GetMethodID fail and the hook silently never installs, which would
        // leave the register body unrewritten with only a debug line to show
        // for it. Confirmed by DEX read on BOTH builds: X.087U.post and
        // X.085T.post both declare (String,Map,byte[])String.
        //
        // Arg indices are LSPlant's, i.e. they include the receiver: [0]=this,
        // [1]=url, [2]=headers Map, [3]=body byte[].
        //
        // Both class names are per-build. The pairing is structural, not
        // guessed: each pair has an identical method set, differing only in the
        // names of the types mentioned.
        //
        //   musically 47.1.4 : LX/087U; (6 methods)  LX/085T; (4 methods)
        //   trill     47.0.3 : LX/07WK; (6 methods)  LX/07Tq; (4 methods)
        //
        // The 6-method one is the base client with the LIZIZ/LIZJ helpers; the
        // 4-method one is the plain variant. Both declare post as
        // (String,Map,byte[])String in both builds.
        { "X.087U", "post", "(Ljava/lang/String;Ljava/util/Map;[B)Ljava/lang/String;",
          false, M_REWRITE_BODY, -1, nullptr, false, 0, V_NONE, -1, 3, 1,
          "/device_register/" },
        { "X.085T", "post", "(Ljava/lang/String;Ljava/util/Map;[B)Ljava/lang/String;",
          false, M_REWRITE_BODY, -1, nullptr, false, 0, V_NONE, -1, 3, 1,
          "/device_register/" },
        { "X.07WK", "post", "(Ljava/lang/String;Ljava/util/Map;[B)Ljava/lang/String;",
          false, M_REWRITE_BODY, -1, nullptr, false, 0, V_NONE, -1, 3, 1,
          "/device_register/" },
        { "X.07Tq", "post", "(Ljava/lang/String;Ljava/util/Map;[B)Ljava/lang/String;",
          false, M_REWRITE_BODY, -1, nullptr, false, 0, V_NONE, -1, 3, 1,
          "/device_register/" },

        // Keva: the SDK's own key/value cache, which is where the persisted
        // openudid/clientudid/cdid are read back from on a warm start.
        //
        // ONE spec per class, not three. LSPlant refuses a second hook on an
        // already-hooked ArtMethod, so three specs on this single getString
        // installed only the first and left clientudid and cdid leaking —
        // verified on device: three specs produced exactly one DoHook line.
        // M_VALUE_MULTI answers all three keys from one hook.
        //
        // getString is an INSTANCE method, so keyArgIndex is 1 (args[0] is the
        // adapter itself). Index 0 would test the receiver, never match, and
        // leave the stored value in place.
        { "com.bytedance.keva.adapter.KevaSpFastAdapter", "getString",
          "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
          false, M_VALUE_MULTI, 1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr,
          kKevaKeys, kKevaVals, kKevaKeyCount },
        // The obfuscated sibling adapter. The LSPosed variant hooks both and
        // records that patching only one left identity fields leaking, so the
        // same two are hooked here.
        //
        // The name is per-build, and the two builds' classes are NOT the same
        // thing under a different name — in trill 47.0.3, X.02tJ is a five-
        // method enum and has no getString at all. The real twin is X.02t3:
        //
        //   musically 47.1.4 : LX/02tJ;  18 methods (full SharedPreferences)
        //   trill     47.0.3 : LX/02t3;  18 methods (full SharedPreferences)
        //
        // Verified by comparing the two method sets: identical except for three
        // parameter types that were renamed with everything else
        // (LX/03a2;->LX/03ax;, LX/05ZB;->LX/05Vg;, LX/05ZU;->LX/05Vz;).
        { "X.02tJ", "getString",
          "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
          false, M_VALUE_MULTI, 1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr,
          kKevaKeys, kKevaVals, kKevaKeyCount },
        { "X.02t3", "getString",
          "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
          false, M_VALUE_MULTI, 1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr,
          kKevaKeys, kKevaVals, kKevaKeyCount },

        // WebView UA, app-side cache. The class that writes ad_user_agent_sp is
        // the ONLY one in the app that does (verified by a const-string scan of
        // all 15 DEX files: exactly one instruction site). Its flow is:
        //   pref "ad_user_agent_sp" non-empty -> return it   (cached, real)
        //   otherwise -> System.getProperty("http.agent")   (real, seeded by
        //                RuntimeInit before this module's hooks would matter)
        // so both branches hand back the real device UA. Rewriting the return
        // value fixes the read path AND the value the app re-persists, which is
        // what a read-path-only fix cannot do for an already-populated cache.
        //
        // The class NAME is not stable across the two installed builds, and the
        // old comment claiming "X.04v0" was verified against "47.1.4" had it
        // backwards — it is the 47.0.3 name:
        //
        //   trill 47.0.3     : LX/04v0;->LIZ(String)String   <- the writer
        //   musically 47.1.4 : LX/04vW;->LIZ(String)String   <- the writer
        //                      LX/04v0;->LIZ(Context)V       <- unrelated
        //
        // So the single spec below installed on trill and MISSED on musically,
        // where X.04v0 exists but with a different signature — the one shape of
        // drift that a class-load observer cannot notice, because the class is
        // present and only the method is wrong. Both names are listed; the
        // signature does the discriminating.
        { "X.04v0", "LIZ", "(Ljava/lang/String;)Ljava/lang/String;",
          true, M_REWRITE_STRING, -1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
        { "X.04vW", "LIZ", "(Ljava/lang/String;)Ljava/lang/String;",
          true, M_REWRITE_STRING, -1, nullptr, false, 0, V_NONE, -1, -1, -1, nullptr },
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
    // sbx_find_class resolves through the app's loadClass, which is itself one
    // of our hooked methods, so a resolution in flight re-enters here. The
    // in_observe flag does not cover that: the re-entry arrives on the same
    // thread but through a different frame, and hooking a class the resolution
    // is already installing would be refused by LSPlant and logged as a miss.
    if (g_in_resolve) return;
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

// Invoked when an application creates its Context/Application instance.
// Resolves application-layer classes directly through the app's ClassLoader.
inline bool sbx_on_app_created(JNIEnv* env, jobject clLoader) {
    if (!env || !clLoader) return false;

    static std::atomic<bool> s_app_hooked{false};
    if (s_app_hooked.load()) return true;

    // Record the app's loader before anything else. This is what makes an
    // app-bundled class resolvable at all (sbx_find_class), and it is the only
    // moment the loader is handed to us directly rather than guessed from a
    // Context.
    if (!g.app_loader) g.app_loader = env->NewGlobalRef(clLoader);

    jclass clClass = env->GetObjectClass(clLoader);
    if (!clClass) return false;
    jmethodID loadClassMid = env->GetMethodID(clClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!loadClassMid || env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }

    // Retry the framework specs that failed at postAppSpecialize, now that
    // sbx_find_class has a loader to fall back to. This is the path that
    // installs com/google/android/gms/ads/identifier/AdvertisingIdClient$Info,
    // which is bundled in the app's own DEX and therefore invisible to
    // FindClass and to the class-load observer alike. Successful entries are
    // dropped; the rest stay queued for a later attach.
    int retried = 0;
    {
        auto& pend = sbx_pending();
        for (size_t i = 0; i < pend.size();) {
            if (hook_one(env, *pend[i])) {
                pend.erase(pend.begin() + (long)i);
                ++retried;
            } else {
                ++i;
            }
        }
    }
    if (retried > 0)
        SBX_LSP_LOGD("L3: %d framework target(s) hooked on app attach", retried);

    // This loop is the ONLY place a tt_specs entry is installed from here.
    //
    // It must not also be done by the class-load observer on the way through.
    // ClassLoader.loadClass(String) delegates to loadClass(String,boolean),
    // which is one of our hooked methods, so every load below re-enters
    // observeAndPass -> sbx_observe, and the observer would hook the spec for
    // the class this loop is about to hook. LSPlant refuses the second hook on
    // an already-hooked ArtMethod, so the loop's own call would fail and the
    // "N app target(s) hooked via onAppCreated" line could never be printed -
    // which is exactly what every session showed before this. g_in_resolve
    // makes the observer a no-op for the duration of the load, leaving this
    // loop as the one owner of the hook.
    size_t n = 0;
    const HookSpec* specs = tt_specs(n);
    int good = 0;
    for (size_t i = 0; i < n; ++i) {
        // Only load classes that have package identifiers (contain dot or slash).
        // Suffix-only legacy specs cannot be loaded via ClassLoader.loadClass.
        if (!strchr(specs[i].cls, '.') && !strchr(specs[i].cls, '/')) continue;

        std::string dot_name(specs[i].cls);
        for (char& c : dot_name) { if (c == '/') c = '.'; }
        jstring name = env->NewStringUTF(dot_name.c_str());
        if (!name) continue;
        g_in_resolve = true;
        jobject clsObj = env->CallObjectMethod(clLoader, loadClassMid, name);
        g_in_resolve = false;
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            continue;
        }
        if (clsObj) {
            jclass cls = (jclass)clsObj;
            if (hook_one_class(env, cls, specs[i])) {
                ++good;
            }
        }
    }
    // Latch on either kind of success. Testing only `good` would leave the
    // latch open when the retry pass was the one that landed something, and
    // the attach hook would then re-enter on every attachBaseContext - each
    // pass re-running a full tt_specs resolution for nothing.
    if (good > 0 || retried > 0) {
        s_app_hooked.store(true);
        if (good > 0)
            SBX_LSP_LOGD("L3: %d app target(s) hooked via onAppCreated", good);
        return true;
    }
    return false;
}

// Force Class.forName(String) to run interpreted.
//
// The 3-arg overload is the deny choke point, and it is reached through this
// one: forName(String) resolves the caller's loader and then invoke-statics the
// 3-arg. Two inlining facts decide whether that call still passes our patch:
//   * forName(String) is @CallerSensitive, so dex2oat must not inline it into
//     its callers - the caller's own class is the whole point of the overload.
//   * Nothing stops dex2oat from inlining the 3-arg *into forName(String)*.
//     If it did, the app's forName("miui.os.Build") would reach classForName
//     directly and never touch the entry point we patched.
// Deoptimizing the 1-arg makes it interpret again, and the interpreter's
// invoke-static goes through that entry point. Harmless when it was never
// inlined; it only costs one interpreted frame on a by-name load.
inline void deopt_forname_1arg(JNIEnv* env) {
    jclass cls = env->FindClass("java/lang/Class");
    if (!cls || env->ExceptionCheck()) { env->ExceptionClear(); return; }
    jmethodID mid = env->GetStaticMethodID(cls, "forName",
                                           "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!mid || env->ExceptionCheck()) { env->ExceptionClear(); return; }
    jobject m = env->ToReflectedMethod(cls, mid, JNI_TRUE);
    if (m) (void)lsplant::Deoptimize(env, m);
    if (env->ExceptionCheck()) env->ExceptionClear();
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

    // Denied OEM classes, as a static field on the callback class. Done here
    // rather than per-hooker because the forName hook is process-wide and the
    // list belongs to the persona, not to the hooked method.
    if (!v.deny_classes.empty()) {
        jclass strCls = env->FindClass("java/lang/String");
        if (strCls) {
            jobjectArray arr = env->NewObjectArray((jsize)v.deny_classes.size(),
                                                   strCls, nullptr);
            if (arr) {
                for (size_t i = 0; i < v.deny_classes.size(); ++i) {
                    jstring s = env->NewStringUTF(v.deny_classes[i].c_str());
                    if (s) env->SetObjectArrayElement(arr, (jsize)i, s);
                }
                env->SetStaticObjectField(g.cb_class, g.f_deny, arr);
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    size_t n = 0;
    const HookSpec* specs = hook_specs(n);
    int good = 0;
    sbx_pending().clear();
    for (size_t i = 0; i < n; ++i) {
        if (hook_one(env, specs[i])) ++good;
        else sbx_pending().push_back(&specs[i]);
    }

    // Only meaningful once the 3-arg deny is actually installed.
    if (!v.deny_classes.empty()) deopt_forname_1arg(env);

    SBX_LSP_LOGD("L3 install_all: %d/%zu targets hooked", good, n);
    if (good == 0) SBX_LSP_LOGE("L3: no targets hooked (continuing with L1/L2/L9)");
    return good > 0;
}
#endif // SBX_ENABLE_LSPLANT

} // namespace sbxlsp
