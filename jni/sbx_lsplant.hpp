#pragma once
#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>
#include <cstdint>

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
// ============================ LSPlant-enabled build ============================
#include <dobby.h>
#include <lsplant.hpp>
#include <lsparself.hpp>
#include "sbx_ident_synth.hpp"   // sbxid::synth_all + sbxnr:: primitives (top-level namespaces)
#if __has_include("hook_dex.h")
#include "hook_dex.h"
#define SBX_HAVE_HOOK_DEX 1
#endif
#endif // SBX_ENABLE_LSPLANT

namespace sbxlsp {

// Values the module feeds to the L3 hooks. main.cpp fills these from the identity
// blob (val()); the telephony/DRM identifiers are synthesized from `seed` so they
// rotate together with the persona on every action.sh run. Defined in BOTH build
// configurations so the no-op stub path keeps the same signature.
struct HookValues {
    std::string android_id;
    std::string serial;
    std::string wifi_mac;
    std::string bt_addr;    // bluetooth MAC, e.g. "A1:B2:C3:D4:E5:F6" (empty => seed fallback)
    std::string op_num;     // gsm operator numeric (MCC+MNC), e.g. "51010"
    std::string op_alpha;   // operator display name, e.g. "Telkomsel"
    std::string op_iso;     // operator country iso, e.g. "id"
    std::string carrier_id; // android carrier id (int as string), e.g. "1435" (empty => skip)
    std::string gaid;       // Google Advertising ID (lowercase UUID); empty => synth from seed
    std::string app_set_id; // App Set ID (lowercase UUID, per-app scope); empty => synth from seed
    uint64_t    seed = 0;   // fnv1a(FINGERPRINT|SERIAL|ANDROID_ID)
};

#ifndef SBX_ENABLE_LSPLANT
// ---- Build without LSPlant: everything compiles to no-ops (validate.sh path) ----
inline bool available()                              { return false; }
inline bool init(JNIEnv*)                            { return false; }
inline bool install_all(JNIEnv*, const HookValues&)  { return false; }

#else

inline bool available() { return true; }

inline void* sbx_inline_hooker(void* target, void* hooker) {
    void* origin = nullptr;
    if (DobbyHook(target, hooker, &origin) == 0) return origin;
    return nullptr;
}
inline bool sbx_inline_unhooker(void* func) { return DobbyDestroy(func) == 0; }

inline bool init(JNIEnv* env) {
    if (!env) return false;
    static bool done = false, ok = false;
    if (done) return ok;
    done = true;

    static lsparself::Elf art("/libart.so");

    static const std::string kCls = "androidx.core.os.HandlerCompatRef";
    static const std::string kSrc = "Hc";
    static const std::string kFld = "h";

    lsplant::InitInfo info{
        .inline_hooker   = sbx_inline_hooker,
        .inline_unhooker = sbx_inline_unhooker,
        .art_symbol_resolver =
            [](std::string_view s) -> void* { return reinterpret_cast<void*>(art.getSymbAddress(s)); },
        // NOTE: LSPlant v2.0 (the pinned LSPLANT_REF) has no art_symbol_prefix_resolver;
        // it was added post-v2.0. v2.0 resolves ART symbols fine without it, so we don't
        // wire lsparself::getSymbPrefixFirstAddress here. Re-add if LSPLANT_REF is bumped.
    };
    info.generated_class_name  = kCls;
    info.generated_source_name = kSrc;
    info.generated_field_name  = kFld;

    ok = lsplant::Init(env, info);
    if (!ok) SBX_LSP_LOGE("lsplant::Init failed — L3 disabled this process (L1/L2 tetap)");
    else     SBX_LSP_LOGD("lsplant::Init ok");
    return ok;
}

// ---- callback class (androidx.core.os.EnvCompatState) loaded from embedded DEX ----
inline jclass    g_cb_class = nullptr;
inline jmethodID g_cb_ctor  = nullptr;   // ()V
inline jmethodID g_cb_handle= nullptr;   // ([Ljava/lang/Object;)Ljava/lang/Object;
inline jobject   g_cb_reflected = nullptr; // reflected handle Method (global ref)
inline jfieldID  f_isStatic=nullptr, f_keyIdx=nullptr, f_keyMatch=nullptr,
                 f_retType=nullptr, f_sval=nullptr, f_bval=nullptr, f_backup=nullptr;
inline std::vector<jobject> g_keep;      // global refs to hookers+backups (GC anchor)

inline jclass load_callback_class(JNIEnv* env) {
#ifndef SBX_HAVE_HOOK_DEX
    (void)env;   // no DEX embedded: nothing to load
    SBX_LSP_LOGE("L3: callback DEX (hook_dex.h) tak ada di build ini — hook dilewati");
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
    jclass g = cls ? (jclass)env->NewGlobalRef(cls) : nullptr;
    env->PopLocalFrame(nullptr);
    return g;
#endif
}

// Resolve ctor / handle / reflected-handle / all field ids once. Returns false on any miss.
inline bool resolve_callback_members(JNIEnv* env) {
    if (!g_cb_class) return false;
    g_cb_ctor   = env->GetMethodID(g_cb_class, "<init>", "()V");
    g_cb_handle = env->GetMethodID(g_cb_class, "handle",
                                   "([Ljava/lang/Object;)Ljava/lang/Object;");
    if (!g_cb_ctor || !g_cb_handle || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    jobject refl = env->ToReflectedMethod(g_cb_class, g_cb_handle, JNI_FALSE);
    if (!refl || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    g_cb_reflected = env->NewGlobalRef(refl);

    f_isStatic = env->GetFieldID(g_cb_class, "isStatic",    "Z");
    f_keyIdx   = env->GetFieldID(g_cb_class, "keyArgIndex", "I");
    f_keyMatch = env->GetFieldID(g_cb_class, "keyMatch",    "Ljava/lang/String;");
    f_retType  = env->GetFieldID(g_cb_class, "retType",     "I");
    f_sval     = env->GetFieldID(g_cb_class, "sval",        "Ljava/lang/String;");
    f_bval     = env->GetFieldID(g_cb_class, "bval",        "[B");
    f_backup   = env->GetFieldID(g_cb_class, "backup",      "Ljava/lang/reflect/Method;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return f_isStatic && f_keyIdx && f_keyMatch && f_retType && f_sval && f_bval && f_backup;
}

// ---- which spoof value a target returns ----
enum ValId {
    V_NONE = 0, V_ANDROID_ID, V_SERIAL, V_IMEI, V_MEID, V_IMSI, V_ICCID,
    V_OP_NUM, V_OP_ALPHA, V_OP_ISO, V_WIFI_MAC, V_BT_ADDR, V_WIDEVINE,
    // SIM-presence gating: without these an app on a SIM-less device sees
    // SIM_STATE_ABSENT and never bothers reading operator/IMSI/ICCID, so those
    // hooks never fire. These return coherent constants (int/boolean).
    V_SIM_STATE, V_PHONE_TYPE, V_ROAMING, V_MODEM_COUNT, V_CARRIER_ID,
    // AdServices (Privacy Sandbox, API 34+) platform identifiers. AdId/AppSetId
    // are framework classes resolvable at postAppSpecialize, so they install
    // immediately (fail-soft skip on older APIs). The GMS play-services
    // equivalents (com.google.android.gms.appset.AppSetIdInfo,
    // AdvertisingIdClient$Info) load from the app dex only AFTER
    // postAppSpecialize and need a deferred installer — see install_all() — so
    // they are intentionally NOT listed here.
    V_GAID, V_APP_SET_ID, V_LAT,
    // SubscriptionManager read path. Apps enumerate SIMs and read identity off the
    // returned SubscriptionInfo; MCC/MNC split out of op_num so getMcc()/getMccString()
    // and the per-slot getters stay coherent with GSM_OPERATOR_NUMERIC (V_OP_NUM).
    V_MCC_STR, V_MNC_STR
};
// retType: 0 String, 1 byte[], 2 int, 3 long, 4 boolean, 5 CharSequence
struct HookSpec {
    const char* cls;
    const char* name;
    const char* sig;
    bool        is_static;
    int         key_index;   // absolute index into args[] (incl. receiver), or -1
    const char* key_match;   // required value at key_index, or nullptr
    int         ret_type;    // 0 String, 1 byte[]
    int         val_id;
};

// Targets resolvable in an ordinary app process at postAppSpecialize. Anything
// absent on the running API (wrong overload / hidden) simply fails resolution
// and is skipped — the loop is fail-soft.
inline const HookSpec* hook_specs(size_t& n) {
    static const HookSpec S[] = {
        // ---- P0: the classic device identifiers ----
        { "android/provider/Settings$Secure", "getString",
          "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",
          true, 1, "android_id", 0, V_ANDROID_ID },
        { "android/os/Build", "getSerial", "()Ljava/lang/String;",
          true, -1, nullptr, 0, V_SERIAL },
        { "android/media/MediaDrm", "getPropertyByteArray", "(Ljava/lang/String;)[B",
          false, 1, "deviceUniqueId", 1, V_WIDEVINE },

        // ---- P1: TelephonyManager (all instance methods) ----
        { "android/telephony/TelephonyManager", "getDeviceId", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_IMEI },
        { "android/telephony/TelephonyManager", "getDeviceId", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_IMEI },
        { "android/telephony/TelephonyManager", "getImei", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_IMEI },
        { "android/telephony/TelephonyManager", "getImei", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_IMEI },
        { "android/telephony/TelephonyManager", "getMeid", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_MEID },
        { "android/telephony/TelephonyManager", "getMeid", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_MEID },
        { "android/telephony/TelephonyManager", "getSubscriberId", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_IMSI },
        { "android/telephony/TelephonyManager", "getSimSerialNumber", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_ICCID },
        { "android/telephony/TelephonyManager", "getNetworkOperator", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_NUM },
        { "android/telephony/TelephonyManager", "getSimOperator", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_NUM },
        { "android/telephony/TelephonyManager", "getNetworkOperatorName", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_ALPHA },
        { "android/telephony/TelephonyManager", "getSimOperatorName", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_ALPHA },
        { "android/telephony/TelephonyManager", "getSimCountryIso", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_ISO },
        { "android/telephony/TelephonyManager", "getNetworkCountryIso", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_ISO },

        // ---- P1: TelephonyManager per-subId overloads (mostly @hide; fail-soft) ----
        // Apps holding a subId (from SubscriptionManager) read these instead of the
        // no-arg forms. Same spoof values so both surfaces agree; keyArgIndex=-1 so
        // any subId gets the single presented operator. getNetworkCountryIso(I) is
        // public since API 30; the rest are greylisted and skip when blocked.
        { "android/telephony/TelephonyManager", "getSubscriberId", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_IMSI },
        { "android/telephony/TelephonyManager", "getSimSerialNumber", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_ICCID },
        { "android/telephony/TelephonyManager", "getSimOperator", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_NUM },
        { "android/telephony/TelephonyManager", "getNetworkCountryIso", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_ISO },

        // ---- P1: SIM-presence gating (int/boolean constants) ----
        // Make a SIM-less device look like it has a ready GSM SIM so apps
        // proceed to the operator/IMSI/ICCID reads above. Overloads/hidden
        // methods absent on the running API simply fail resolution and skip.
        { "android/telephony/TelephonyManager", "getSimState", "()I",
          false, -1, nullptr, 2, V_SIM_STATE },
        { "android/telephony/TelephonyManager", "getSimState", "(I)I",
          false, -1, nullptr, 2, V_SIM_STATE },
        { "android/telephony/TelephonyManager", "getPhoneType", "()I",
          false, -1, nullptr, 2, V_PHONE_TYPE },
        { "android/telephony/TelephonyManager", "isNetworkRoaming", "()Z",
          false, -1, nullptr, 4, V_ROAMING },
        { "android/telephony/TelephonyManager", "getPhoneCount", "()I",
          false, -1, nullptr, 2, V_MODEM_COUNT },
        { "android/telephony/TelephonyManager", "getActiveModemCount", "()I",
          false, -1, nullptr, 2, V_MODEM_COUNT },
        { "android/telephony/TelephonyManager", "getSimCarrierId", "()I",
          false, -1, nullptr, 2, V_CARRIER_ID },

        // ---- P1: carrier-id-name / specific-carrier-id (API 28/29) ----
        // The name getters return CharSequence (retType 5 => our String is a
        // CharSequence, assignment-compatible). getSimSpecificCarrierId mirrors the
        // canonical carrier id; empty carrier_id (unverified operator) => spoofed as
        // UNKNOWN_CARRIER_ID (-1) rather than passthrough, see sbx_value_for().
        { "android/telephony/TelephonyManager", "getSimCarrierIdName", "()Ljava/lang/CharSequence;",
          false, -1, nullptr, 5, V_OP_ALPHA },
        { "android/telephony/TelephonyManager", "getSimSpecificCarrierId", "()I",
          false, -1, nullptr, 2, V_CARRIER_ID },
        { "android/telephony/TelephonyManager", "getSimSpecificCarrierIdName", "()Ljava/lang/CharSequence;",
          false, -1, nullptr, 5, V_OP_ALPHA },

        // ---- P1: SubscriptionInfo getters (the SubscriptionManager read path) ----
        // Apps enumerate SIMs via SubscriptionManager.getActiveSubscriptionInfoList()
        // and read identity off each SubscriptionInfo. Hooking the object's getters
        // covers every caller regardless of how the SubscriptionInfo was obtained and
        // keeps MCC/MNC/carrier/iso coherent with the TelephonyManager surface above —
        // a mismatch between the two is a primary tampering tell. getMcc/getMnc (int,
        // deprecated) and getMccString/getMncString derive from the same op_num split.
        { "android/telephony/SubscriptionInfo", "getMccString", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_MCC_STR },
        { "android/telephony/SubscriptionInfo", "getMncString", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_MNC_STR },
        { "android/telephony/SubscriptionInfo", "getMcc", "()I",
          false, -1, nullptr, 2, V_MCC_STR },
        { "android/telephony/SubscriptionInfo", "getMnc", "()I",
          false, -1, nullptr, 2, V_MNC_STR },
        { "android/telephony/SubscriptionInfo", "getCountryIso", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_ISO },
        { "android/telephony/SubscriptionInfo", "getCarrierName", "()Ljava/lang/CharSequence;",
          false, -1, nullptr, 5, V_OP_ALPHA },
        { "android/telephony/SubscriptionInfo", "getDisplayName", "()Ljava/lang/CharSequence;",
          false, -1, nullptr, 5, V_OP_ALPHA },
        { "android/telephony/SubscriptionInfo", "getCarrierId", "()I",
          false, -1, nullptr, 2, V_CARRIER_ID },
        { "android/telephony/SubscriptionInfo", "getIccId", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_ICCID },

        // ---- P1: MAC addresses ----
        { "android/net/wifi/WifiInfo", "getMacAddress", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_WIFI_MAC },
        { "android/bluetooth/BluetoothAdapter", "getAddress", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_BT_ADDR },

        // ---- P2: AdServices (Privacy Sandbox, API 34+) app-set-id / advertising-id ----
        // Platform classes, so hookable at install time on API 34+ (fail-soft skip
        // otherwise). getAdId()/getId() return the spoofed UUIDs; forcing
        // isLimitAdTrackingEnabled=false keeps the advertising id coherent — a
        // limit-ad-tracking device is required to report the all-zero id.
        { "android/adservices/adid/AdId", "getAdId", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_GAID },
        { "android/adservices/adid/AdId", "isLimitAdTrackingEnabled", "()Z",
          false, -1, nullptr, 4, V_LAT },
        { "android/adservices/appsetid/AppSetId", "getId", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_APP_SET_ID },
    };
    n = sizeof(S) / sizeof(S[0]);
    return S;
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

// Hook exactly one target. Fail-soft: a missing class/method/overload returns false
// (skipped) rather than aborting the whole install. `sval` may be empty (=> the hook
// is registered but passes through until a value exists); `wvbytes` is the hex string
// for byte[] targets (V_WIDEVINE) or empty.
inline bool hook_one(JNIEnv* env, const HookSpec& sp,
                     const std::string& sval, const std::string& wvbytes) {
    if (env->PushLocalFrame(24) != 0) { env->ExceptionClear(); return false; }
    bool ok = [&]() -> bool {
        jclass cls = env->FindClass(sp.cls);
        if (!cls || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        jmethodID mid = sp.is_static ? env->GetStaticMethodID(cls, sp.name, sp.sig)
                                     : env->GetMethodID(cls, sp.name, sp.sig);
        if (!mid || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        jobject target = env->ToReflectedMethod(cls, mid, sp.is_static ? JNI_TRUE : JNI_FALSE);
        if (!target || env->ExceptionCheck()) { env->ExceptionClear(); return false; }

        jobject hooker = env->NewObject(g_cb_class, g_cb_ctor);
        if (!hooker || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        env->SetBooleanField(hooker, f_isStatic, sp.is_static ? JNI_TRUE : JNI_FALSE);
        env->SetIntField(hooker, f_keyIdx, sp.key_index);
        env->SetIntField(hooker, f_retType, sp.ret_type);
        if (sp.key_match) {
            jstring km = env->NewStringUTF(sp.key_match);
            env->SetObjectField(hooker, f_keyMatch, km);
        }
        if (sp.ret_type == 1) {                      // byte[] target (Widevine)
            if (wvbytes.size() >= 2) {
                jbyteArray b = sbx_hex_to_jbytes(env, wvbytes);
                if (b) env->SetObjectField(hooker, f_bval, b);
            }
        } else if (!sval.empty()) {                  // String target
            jstring sv = env->NewStringUTF(sval.c_str());
            env->SetObjectField(hooker, f_sval, sv);
        }

        jobject backup = lsplant::Hook(env, target, hooker, g_cb_reflected);
        if (!backup || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        env->SetObjectField(hooker, f_backup, backup);

        g_keep.push_back(env->NewGlobalRef(hooker));
        g_keep.push_back(env->NewGlobalRef(backup));

        lsplant::Deoptimize(env, target);            // best-effort: defeat inlined callers
        if (env->ExceptionCheck()) env->ExceptionClear();
        return true;
    }();
    env->PopLocalFrame(nullptr);
    if (ok) SBX_LSP_LOGD("L3 hooked %s.%s%s", sp.cls, sp.name, sp.sig);
    return ok;
}

inline std::string sbx_value_for(int val_id, const HookValues& v,
                                 const sbxid::SynthIds& ids,
                                 const std::string& wifi, const std::string& bt,
                                 const std::string& gaid, const std::string& appset) {
    switch (val_id) {
        case V_ANDROID_ID: return v.android_id;
        case V_SERIAL:     return v.serial;
        case V_IMEI:       return ids.imei;
        case V_MEID:       return ids.meid;
        case V_IMSI:       return ids.imsi;
        case V_ICCID:      return ids.iccid;
        case V_OP_NUM:     return v.op_num;
        // MCC/MNC split out of the operator numeric (MCC = first 3 digits). Feeds both
        // the String getters (retType 0) and the deprecated int getters (retType 2,
        // parsed Java-side; leading-zero MNC like "08" parses to 8 as the framework
        // returns it). Empty op_num => empty => hook passes through to the real method.
        case V_MCC_STR:    return v.op_num.size() >= 3 ? v.op_num.substr(0, 3) : std::string();
        case V_MNC_STR:    return v.op_num.size() >  3 ? v.op_num.substr(3)    : std::string();
        case V_OP_ALPHA:   return v.op_alpha;
        case V_OP_ISO:     return v.op_iso;
        case V_WIFI_MAC:   return wifi;
        case V_BT_ADDR:    return bt;
        // SIM-presence constants (parsed int/boolean on the Java side):
        case V_SIM_STATE:  return "5";       // TelephonyManager.SIM_STATE_READY
        case V_PHONE_TYPE: return "1";       // PHONE_TYPE_GSM
        case V_ROAMING:    return "false";
        case V_MODEM_COUNT:return "1";
        // Empty carrier_id means the operator (e.g. Tri/Smartfren) has no verified
        // AOSP carrier_id, but an empty sval here would make the Java-side hook
        // fall through to invokeOriginal() and leak the REAL device's carrier_id
        // alongside our spoofed MCC/MNC — a worse tell than reporting UNKNOWN.
        // Force TelephonyManager.UNKNOWN_CARRIER_ID (-1) instead whenever a SIM
        // persona is active; only pass through when there is no SIM at all
        // (handled by the have_sim gate in install_all()).
        case V_CARRIER_ID: return v.carrier_id.empty() ? std::string("-1") : v.carrier_id;
        // AdServices (API 34+) platform identifiers:
        case V_GAID:       return gaid;
        case V_APP_SET_ID: return appset;
        case V_LAT:        return "false";        // isLimitAdTrackingEnabled
        default:           return std::string();
    }
}

inline bool install_all(JNIEnv* env, const HookValues& v) {
    if (!env) return false;
    if (!init(env)) return false;

    if (!g_cb_class) {
        g_cb_class = load_callback_class(env);
        if (!g_cb_class || !resolve_callback_members(env)) {
            SBX_LSP_LOGE("L3: callback class/members unavailable — hooks skipped");
            return false;
        }
    }

    // Telephony/DRM identifiers, synthesized from the persona seed for cross-field
    // consistency and rotated with every fresh identity.
    sbxid::SynthIds ids = sbxid::synth_all(v.seed, v.op_num);
    // WiFi/BT MACs: prefer the persisted persona values; fall back to the seed.
    // The WiFi salt matches L9 (main.cpp) so both layers agree if WIFI_MAC is absent.
    std::string wifi = !v.wifi_mac.empty() ? v.wifi_mac
                                           : sbxnr::mac_from_seed(v.seed ^ 0x9E3779B97F4A7C15ULL);
    std::string bt   = !v.bt_addr.empty() ? sbx_mac_upper(v.bt_addr)
                                          : sbx_mac_upper(sbxnr::mac_from_seed(v.seed ^ 0x424C554554ULL));
    // AdServices UUIDs: prefer the persisted persona values, else derive from the
    // seed so they rotate with every fresh identity (distinct salts => distinct
    // from each other and from boot_id/cdid/clientudid at L9).
    std::string gaid   = !v.gaid.empty() ? v.gaid
                                         : sbxnr::uuid_from_seed(v.seed ^ 0x47414944ULL);   // "GAID"
    std::string appset = !v.app_set_id.empty() ? v.app_set_id
                                               : sbxnr::uuid_from_seed(v.seed ^ 0x4150534554ULL); // "APSET"

    // NOTE (deferred install, future work): the GMS play-services getters
    // com.google.android.gms.appset.AppSetIdInfo.getId()/getScope() and
    // com.google.android.gms.ads.identifier.AdvertisingIdClient$Info.getId()/
    // isLimitAdTrackingEnabled() are the identifiers most apps actually read, but
    // those classes live in the app's own dex and are not defined until AFTER
    // postAppSpecialize (the app PathClassLoader does not exist yet here), so
    // FindClass misses them and they cannot be listed in hook_specs(). Installing
    // them needs a class-load watch (hook dalvik.system.BaseDexClassLoader.findClass
    // with a dedicated post-callback that, on a name match, calls back to native to
    // hook the getters) — a mechanism that must be validated on a device before it
    // ships, since a fault in a findClass hook would break class loading app-wide.
    // The AdServices (API 34+) platform path above is the safe, framework-only
    // subset that needs no deferral.

    const bool have_sim = !v.op_num.empty();
    size_t n = 0;
    const HookSpec* specs = hook_specs(n);
    int good = 0;
    for (size_t i = 0; i < n; ++i) {
        const int vid = specs[i].val_id;
        // The SIM-presence constants are only coherent when we actually present
        // an operator; without one, let SIM state pass through (real ABSENT).
        if (!have_sim && (vid == V_SIM_STATE || vid == V_MODEM_COUNT || vid == V_CARRIER_ID))
            continue;
        std::string sval = sbx_value_for(vid, v, ids, wifi, bt, gaid, appset);
        if (hook_one(env, specs[i], sval, ids.widevine_hex)) ++good;
    }
    SBX_LSP_LOGD("L3 install_all: %d/%zu targets hooked", good, n);
    if (good == 0) SBX_LSP_LOGE("L3: no targets hooked (continuing with L1/L2/L9)");
    return good > 0;
}
#endif // SBX_ENABLE_LSPLANT

} // namespace sbxlsp
