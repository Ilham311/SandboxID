#pragma once
#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

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
#include "sbx_ident_synth.hpp"
#include "sbx_native_drm.hpp"
#if __has_include("hook_dex.h")
#include "hook_dex.h"
#define SBX_HAVE_HOOK_DEX 1
#endif
#endif

namespace sbxlsp {

struct HookValues {
    std::string android_id;
    std::string serial;
    std::string wifi_mac;
    std::string bt_addr;
    std::string op_num;
    std::string op_alpha;
    std::string op_iso;
    std::string carrier_id;
    std::string gaid;
    std::string app_set_id;
    uint64_t    seed = 0;
    bool gms_watch = false;

};

#ifndef SBX_ENABLE_LSPLANT

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

    };
    info.generated_class_name  = kCls;
    info.generated_source_name = kSrc;
    info.generated_field_name  = kFld;

    ok = lsplant::Init(env, info);
    if (!ok) SBX_LSP_LOGE("lsplant::Init failed — L3 disabled this process (L1/L2 tetap)");
    else     SBX_LSP_LOGD("lsplant::Init ok");
    return ok;
}

inline jclass    g_cb_class = nullptr;
inline jmethodID g_cb_ctor  = nullptr;
inline jmethodID g_cb_handle= nullptr;
inline jobject   g_cb_reflected = nullptr;
inline jfieldID  f_isStatic=nullptr, f_keyIdx=nullptr, f_keyMatch=nullptr,
                 f_retType=nullptr, f_sval=nullptr, f_bval=nullptr, f_backup=nullptr;
inline std::vector<jobject> g_keep;

inline std::string g_gms_gaid;
inline std::string g_gms_appset;
inline bool        g_gms_adv_done   = false;
inline bool        g_gms_appset_done = false;
inline std::mutex  g_gms_mu;

inline jclass load_callback_class(JNIEnv* env) {
#ifndef SBX_HAVE_HOOK_DEX
    (void)env;
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

enum ValId {
    V_NONE = 0, V_ANDROID_ID, V_SERIAL, V_IMEI, V_MEID, V_IMSI, V_ICCID,
    V_OP_NUM, V_OP_ALPHA, V_OP_ISO, V_WIFI_MAC, V_BT_ADDR, V_WIDEVINE,

    V_SIM_STATE, V_PHONE_TYPE, V_ROAMING, V_MODEM_COUNT, V_CARRIER_ID,

    V_GAID, V_APP_SET_ID, V_LAT,

    V_MCC_STR, V_MNC_STR,

    V_WIFI_SSID, V_WIFI_BSSID,

    V_EMPTY_LIST,

    V_GSERVICES
};

struct HookSpec {
    const char* cls;
    const char* name;
    const char* sig;
    bool        is_static;
    int         key_index;
    const char* key_match;
    int         ret_type;
    int         val_id;
    bool        no_deopt = false;
};

inline const HookSpec* hook_specs(size_t& n) {
    static const HookSpec S[] = {

        { "android/provider/Settings$Secure", "getString",
          "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",
          true, 1, "android_id", 0, V_ANDROID_ID },

        { "android/provider/Settings$Secure", "getString",
          "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",
          true, 1, "bluetooth_address", 0, V_BT_ADDR },
        { "android/os/Build", "getSerial", "()Ljava/lang/String;",
          true, -1, nullptr, 0, V_SERIAL },
        { "android/media/MediaDrm", "getPropertyByteArray", "(Ljava/lang/String;)[B",
          false, 1, "deviceUniqueId", 1, V_WIDEVINE },

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

        { "android/telephony/TelephonyManager", "getSubscriberId", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_IMSI },
        { "android/telephony/TelephonyManager", "getSimSerialNumber", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_ICCID },
        { "android/telephony/TelephonyManager", "getSimOperator", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_NUM },
        { "android/telephony/TelephonyManager", "getNetworkCountryIso", "(I)Ljava/lang/String;",
          false, -1, nullptr, 0, V_OP_ISO },

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

        { "android/telephony/TelephonyManager", "getSimCarrierIdName", "()Ljava/lang/CharSequence;",
          false, -1, nullptr, 5, V_OP_ALPHA },
        { "android/telephony/TelephonyManager", "getSimSpecificCarrierId", "()I",
          false, -1, nullptr, 2, V_CARRIER_ID },
        { "android/telephony/TelephonyManager", "getSimSpecificCarrierIdName", "()Ljava/lang/CharSequence;",
          false, -1, nullptr, 5, V_OP_ALPHA },

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

        { "android/net/wifi/WifiInfo", "getMacAddress", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_WIFI_MAC },
        { "android/bluetooth/BluetoothAdapter", "getAddress", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_BT_ADDR },

        { "android/net/wifi/WifiInfo", "getSSID", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_WIFI_SSID },
        { "android/net/wifi/WifiInfo", "getBSSID", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_WIFI_BSSID },

        { "android/net/wifi/WifiManager", "getConfiguredNetworks", "()Ljava/util/List;",
          false, -1, nullptr, 6, V_EMPTY_LIST },

        { "android/adservices/adid/AdId", "getAdId", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_GAID },
        { "android/adservices/adid/AdId", "isLimitAdTrackingEnabled", "()Z",
          false, -1, nullptr, 4, V_LAT },
        { "android/adservices/appsetid/AppSetId", "getId", "()Ljava/lang/String;",
          false, -1, nullptr, 0, V_APP_SET_ID },

        { "android/content/ContentResolver", "query",
          "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
          false, -1, nullptr, 7, V_GSERVICES, true },
        { "android/content/ContentResolver", "query",
          "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Landroid/os/CancellationSignal;)Landroid/database/Cursor;",
          false, -1, nullptr, 7, V_GSERVICES, true },
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

inline bool hook_one_on_class(JNIEnv* env, jclass cls, const HookSpec& sp,
                              const std::string& sval, const std::string& wvbytes) {
    if (!cls) return false;
    if (env->PushLocalFrame(24) != 0) { env->ExceptionClear(); return false; }
    bool ok = [&]() -> bool {
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
        if (sp.ret_type == 1) {
            if (wvbytes.size() >= 2) {
                jbyteArray b = sbx_hex_to_jbytes(env, wvbytes);
                if (b) env->SetObjectField(hooker, f_bval, b);
            }
        } else if (!sval.empty()) {
            jstring sv = env->NewStringUTF(sval.c_str());
            env->SetObjectField(hooker, f_sval, sv);
        }

        jobject backup = lsplant::Hook(env, target, hooker, g_cb_reflected);
        if (!backup || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        env->SetObjectField(hooker, f_backup, backup);

        g_keep.push_back(env->NewGlobalRef(hooker));
        g_keep.push_back(env->NewGlobalRef(backup));

        if (!sp.no_deopt) {
            lsplant::Deoptimize(env, target);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        return true;
    }();
    env->PopLocalFrame(nullptr);
    if (ok) SBX_LSP_LOGD("L3 hooked %s.%s%s", sp.cls, sp.name, sp.sig);
    return ok;
}

inline bool hook_one(JNIEnv* env, const HookSpec& sp,
                     const std::string& sval, const std::string& wvbytes) {
    if (env->PushLocalFrame(4) != 0) { env->ExceptionClear(); return false; }
    jclass cls = env->FindClass(sp.cls);
    if (!cls || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->PopLocalFrame(nullptr);
        return false;
    }
    bool ok = hook_one_on_class(env, cls, sp, sval, wvbytes);
    env->PopLocalFrame(nullptr);
    return ok;
}

inline void hook_gms_getters(JNIEnv* env, jclass cls, bool is_advertising) {
    if (!cls) return;
    if (is_advertising) {
        HookSpec gid{ "com/google/android/gms/ads/identifier/AdvertisingIdClient$Info",
                      "getId", "()Ljava/lang/String;", false, -1, nullptr, 0, V_NONE, true };
        hook_one_on_class(env, cls, gid, g_gms_gaid, std::string());
        HookSpec lat{ "com/google/android/gms/ads/identifier/AdvertisingIdClient$Info",
                      "isLimitAdTrackingEnabled", "()Z", false, -1, nullptr, 4, V_NONE, true };
        hook_one_on_class(env, cls, lat, "false", std::string());
    } else {
        HookSpec sid{ "com/google/android/gms/appset/AppSetIdInfo",
                      "getId", "()Ljava/lang/String;", false, -1, nullptr, 0, V_NONE, true };
        hook_one_on_class(env, cls, sid, g_gms_appset, std::string());
    }
}

inline void sbx_on_class_loaded(JNIEnv* env, jclass , jstring jname, jobject clsObj) {
    if (!env || !jname || !clsObj) return;
    const char* nm = env->GetStringUTFChars(jname, nullptr);
    if (!nm) { if (env->ExceptionCheck()) env->ExceptionClear(); return; }
    std::string name(nm);
    env->ReleaseStringUTFChars(jname, nm);
    std::lock_guard<std::mutex> lk(g_gms_mu);
    if (!g_gms_adv_done &&
        name == "com.google.android.gms.ads.identifier.AdvertisingIdClient$Info") {
        g_gms_adv_done = true;
        hook_gms_getters(env, static_cast<jclass>(clsObj), true);
    } else if (!g_gms_appset_done &&
               name == "com.google.android.gms.appset.AppSetIdInfo") {
        g_gms_appset_done = true;
        hook_gms_getters(env, static_cast<jclass>(clsObj), false);
    }
}

inline bool register_class_watch_native(JNIEnv* env) {
    if (!g_cb_class) return false;
    JNINativeMethod m{ "onClassLoaded", "(Ljava/lang/String;Ljava/lang/Object;)V",
                       reinterpret_cast<void*>(&sbx_on_class_loaded) };
    if (env->RegisterNatives(g_cb_class, &m, 1) != 0) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }
    return true;
}

inline std::string sbx_value_for(int val_id, const HookValues& v,
                                 const sbxid::SynthIds& ids,
                                 const std::string& wifi, const std::string& bt,
                                 const std::string& gaid, const std::string& appset,
                                 const std::string& gsf) {
    switch (val_id) {
        case V_ANDROID_ID: return v.android_id;
        case V_SERIAL:     return v.serial;
        case V_IMEI:       return ids.imei;
        case V_MEID:       return ids.meid;
        case V_IMSI:       return ids.imsi;
        case V_ICCID:      return ids.iccid;
        case V_OP_NUM:     return v.op_num;

        case V_MCC_STR:    return v.op_num.size() >= 3 ? v.op_num.substr(0, 3) : std::string();
        case V_MNC_STR:    return v.op_num.size() >  3 ? v.op_num.substr(3)    : std::string();
        case V_OP_ALPHA:   return v.op_alpha;
        case V_OP_ISO:     return v.op_iso;
        case V_WIFI_MAC:   return wifi;
        case V_BT_ADDR:    return bt;

        case V_WIFI_SSID:  return "<unknown ssid>";
        case V_WIFI_BSSID: return "02:00:00:00:00:00";
        case V_EMPTY_LIST: return std::string();

        case V_SIM_STATE:  return "5";
        case V_PHONE_TYPE: return "1";
        case V_ROAMING:    return "false";
        case V_MODEM_COUNT:return "1";

        case V_CARRIER_ID: return v.carrier_id.empty() ? std::string("-1") : v.carrier_id;

        case V_GAID:       return gaid;
        case V_APP_SET_ID: return appset;
        case V_LAT:        return "false";

        case V_GSERVICES:  return gsf;
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

    sbxid::SynthIds ids = sbxid::synth_all(v.seed, v.op_num);

    std::string wifi = !v.wifi_mac.empty() ? v.wifi_mac
                                           : sbxnr::mac_from_seed(v.seed ^ 0x9E3779B97F4A7C15ULL);
    std::string bt   = !v.bt_addr.empty() ? sbx_mac_upper(v.bt_addr)
                                          : sbx_mac_upper(sbxnr::mac_from_seed(v.seed ^ 0x424C554554ULL));

    std::string gaid   = !v.gaid.empty() ? v.gaid
                                         : sbxnr::uuid_from_seed(v.seed ^ 0x47414944ULL);
    std::string appset = !v.app_set_id.empty() ? v.app_set_id
                                               : sbxnr::uuid_from_seed(v.seed ^ 0x4150534554ULL);

    std::string gsf = sbxid::synth_gsf_id(v.seed);

    const bool have_sim = !v.op_num.empty();
    size_t n = 0;
    const HookSpec* specs = hook_specs(n);
    int good = 0;
    for (size_t i = 0; i < n; ++i) {
        const int vid = specs[i].val_id;

        if (!have_sim && (vid == V_SIM_STATE || vid == V_MODEM_COUNT || vid == V_CARRIER_ID))
            continue;
        std::string sval = sbx_value_for(vid, v, ids, wifi, bt, gaid, appset, gsf);
        if (hook_one(env, specs[i], sval, ids.widevine_hex)) ++good;
    }

    if (v.gms_watch) {
        g_gms_gaid   = gaid;
        g_gms_appset = appset;
        if (register_class_watch_native(env)) {
            HookSpec fc{ "dalvik/system/BaseDexClassLoader", "findClass",
                         "(Ljava/lang/String;)Ljava/lang/Class;",
                         false, -1, nullptr, 9, V_NONE, true };
            if (hook_one(env, fc, std::string(), std::string())) {
                ++good;
                SBX_LSP_LOGD("L3 GMS class-load watch armed");
            }
        }
    }

    SBX_LSP_LOGD("L3 install_all: %d/%zu targets hooked", good, n);
    if (good == 0) SBX_LSP_LOGE("L3: no targets hooked (continuing with L1/L2/L9)");
    return good > 0;
}
#endif

}
