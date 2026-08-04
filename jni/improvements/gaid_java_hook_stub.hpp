// gaid_java_hook_stub.hpp
//
// OPTIONAL / FUTURE WORK — stub for Priority Matrix item #1 (LSPlant Java hook).
// Pack ini secara default JALAN pakai jalur #2 (settings_secure.xml). Hook Java
// jadi jalur backup buat app yang bypass ContentProvider (rare, tapi ada).
//
// Target method:
//   com.google.android.gms.ads.identifier.AdvertisingIdClient$Info.getId() : String
//   com.google.android.gms.ads.identifier.AdvertisingIdClient$Info.isLimitAdTrackingEnabled() : boolean
//
// Yang TIDAK di-lakukan di stub ini (biar tetep buildable tanpa LSPlant):
//   - inisialisasi LSPlant (butuh vendor lib + init_info)
//   - resolve class via JNI FindClass di ClassLoader Play Services
//
// Pattern integrasi (ketika kamu tambahkan LSPlant nanti):
//
//   1. Load LSPlant di JNI_OnLoad (see LSPlant README).
//   2. Panggil hook_gaid_info(env) setelah system_server ATT_CHILD ke target.
//   3. Callback bawa string GAID dari IDENTITY_FILE (via gaid_persistence.hpp).
//
#pragma once

#include <jni.h>
#include <string>

namespace ttfix {

// Placeholder — real body butuh LSPlant.
inline bool hook_gaid_info(JNIEnv* /*env*/, const std::string& /*gaid_uuid*/) {
    // TODO: ganti dengan implementasi LSPlant.
    // Contoh sketch:
    //   jclass klass = env->FindClass("com/google/android/gms/ads/identifier/AdvertisingIdClient$Info");
    //   jmethodID mid = env->GetMethodID(klass, "getId", "()Ljava/lang/String;");
    //   lsplant::Hook(env, mid,
    //                 &gaid_replacement,
    //                 &gaid_original);
    return false;
}

} // namespace ttfix
