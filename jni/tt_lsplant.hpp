#pragma once
//
// tt_lsplant.hpp — L3: pintu untuk hook method Java NON-native via LSPlant.
//
// KENAPA ADA (ringkas dari riset Fase 1/2):
//   hookJniNativeMethods (L2) HANYA bisa mengikat method `native`. Target
//   bernilai tinggi seperti Settings.Secure.getString BUKAN native, jadi butuh
//   ART method-hooking. LSPlant (github.com/LSPosed/LSPlant) melakukan itu, tapi
//   ia hanya "otak"-nya: backend inline-hook (Dobby) dan resolver simbol
//   libart.so (lsparself) HARUS kita suplai lewat InitInfo.
//
// ⚠️ BOOT RISK — INI ALASAN SELURUH LAPISAN INI DEFAULT-OFF:
//   LSPlant menyentuh internal ArtMethod. Init/Hook yang gagal di ROM/versi
//   Android tertentu bisa meng-crash zygote-child => BOOT LOOP untuk SEMUA app
//   target sekaligus. Karena repo ini di-review tanpa build+boot loop, kita
//   TIDAK boleh mengirim jalur ini dalam keadaan hidup. Maka:
//     1) Semua kode aktif dikurung compile-flag TT_ENABLE_LSPLANT. Default OFF
//        => build release BYTE-IDENTIK dengan v1.0.27; nol risiko sampai flag
//        di-flip DENGAN verifikasi boot per-ABI / per-Android-version.
//     2) Setiap entry fail-safe: kembalikan false + LOGE. JANGAN abort(),
//        JANGAN unload setengah-jadi. L1/L2/(L7) tetap jalan penuh tanpa L3.
//
// STATUS SEAM (dulu placeholder, kini terimplementasi sebagai kode):
//   - SEAM #1 (resolver simbol libart): di-wire ke lsparself, persis test resmi
//     v6.4 (`lsparself::Elf("/libart.so")`). lsparself menangani .gnu_debugdata
//     (mini-symtab LZMA) — sebabnya kita TIDAK hand-roll parser ELF naif yang
//     akan diam-diam gagal menemukan simbol ART internal.
//   - SEAM #2 (callback): kelas Java pure-Java `androidx.core.os.EnvCompatState`
//     (jni/TtHook.java) di-compile ke DEX (d8), di-embed sebagai byte array
//     (jni/tt_hook_dex.h), dimuat runtime via InMemoryDexClassLoader. Tanpa
//     header DEX itu, hook_android_id() fail-safe (LOGE + return false).
//
// CARA MENGAKTIFKAN (butuh compiler + device — lihat CMakeLists.txt & CHANGELOG):
//   1) vendor jni/external/{lsplant@v6.4 (--recurse-submodules), dobby, lsparself}
//   2) bangun jni/tt_hook_dex.h dari jni/TtHook.java via `javac` + `d8`
//   3) `TT_ENABLE_LSPLANT=ON ./build.sh` (pertimbangkan -DANDROID_STL=c++_static),
//      lalu BOOT TEST tiap ABI/Android version sebelum flag di-flip di rilis.
//
#include <jni.h>
#include <android/log.h>
#include <string>

#ifndef TT_LSP_TAG
#define TT_LSP_TAG "TernakTT-L3"
#endif
#define TT_LSP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TT_LSP_TAG, __VA_ARGS__)
#ifdef TT_DEBUG
#define TT_LSP_LOGD(...) __android_log_print(ANDROID_LOG_INFO, TT_LSP_TAG, "[D] " __VA_ARGS__)
#else
#define TT_LSP_LOGD(...) ((void)0)
#endif

// Library eksternal HANYA saat L3 aktif. WAJIB di luar namespace (menyarangkan
// header ke dalam namespace = simbol library ikut ter-nest => broken).
#ifdef TT_ENABLE_LSPLANT
#include <dobby.h>
#include <lsplant.hpp>
#include <lsparself.hpp>
// Callback DEX di-embed saat build (d8 atas jni/TtHook.java). Kalau header belum
// dibuat, TT_HAVE_HOOK_DEX tak terdefinisi => hook_android_id() fail-safe.
#if __has_include("tt_hook_dex.h")
#include "tt_hook_dex.h"        // unsigned char tt_hook_dex[]; unsigned int tt_hook_dex_len;
#define TT_HAVE_HOOK_DEX 1
#endif
#endif  // TT_ENABLE_LSPLANT

namespace ttlsp {

// Nilai spoof yang dibutuhkan callback; disetor SEBELUM hook dipasang. Satu
// proses = satu app, jadi global tanpa lock (sama seperti g_id di main.cpp).
inline std::string g_android_id;
inline void set_android_id(const std::string& v) { g_android_id = v; }

#ifndef TT_ENABLE_LSPLANT
// ======================= L3 DISABLED (default) ==============================
// Stub no-op. main.cpp memanggil ini apa adanya; semua di-inline jadi nol kode.
inline bool available()                               { return false; }
inline bool init(JNIEnv* /*env*/)                     { return false; }
inline bool hook_android_id(JNIEnv* /*env*/,
                            const std::string& /*v*/)  { return false; }

#else
// ======================= L3 ENABLED (experimental) ==========================
// CATATAN: cabang ini TIDAK ikut ter-compile di build default. Ia butuh library
// yang di-vendor (lihat header di atas). Fail-safe di setiap langkah.

inline bool available() { return true; }

// ---- Dobby sebagai inline_hooker / inline_unhooker (sesuai test resmi) ----
inline void* tt_inline_hooker(void* target, void* hooker) {
    void* origin = nullptr;
    if (DobbyHook(target, hooker, &origin) == 0) return origin;
    return nullptr;
}
inline bool tt_inline_unhooker(void* func) {
    return DobbyDestroy(func) == 0;
}

inline bool init(JNIEnv* env) {
    if (!env) return false;
    static bool done = false, ok = false;
    if (done) return ok;              // Init cukup sekali per proses.
    done = true;

    // SEAM #1 — resolver simbol libart via lsparself. "/libart.so" dicocokkan
    // by-suffix ke lib yang sudah ter-load (persis test resmi LSPlant v6.4).
    // static: hidup selama proses; lambda di InitInfo merujuknya tanpa capture
    // (storage statik), dan LSPlant memakainya saat Hook (setelah init selesai).
    static lsparself::Elf art("/libart.so");

    // Nama class/field yang di-generate LSPlant. Default "LSPHooker_"/"hooker"
    // adalah fingerprint LSPosed => diganti nama netral untuk kurangi deteksi.
    static const std::string kCls = "androidx.core.os.HandlerCompatRef";
    static const std::string kSrc = "Hc";
    static const std::string kFld = "h";

    lsplant::InitInfo info{
        .inline_hooker   = tt_inline_hooker,
        .inline_unhooker = tt_inline_unhooker,
        .art_symbol_resolver =
            [](std::string_view s) -> void* { return reinterpret_cast<void*>(art.getSymbAddress(s)); },
        .art_symbol_prefix_resolver =
            [](std::string_view s) -> void* { return reinterpret_cast<void*>(art.getSymbPrefixFirstAddress(s)); },
    };
    info.generated_class_name  = kCls;
    info.generated_source_name = kSrc;
    info.generated_field_name  = kFld;

    ok = lsplant::Init(env, info);
    if (!ok) TT_LSP_LOGE("lsplant::Init failed — L3 disabled this process (L1/L2 tetap)");
    else     TT_LSP_LOGD("lsplant::Init ok");
    return ok;
}

// Ref global supaya class/instance/backup tak di-GC selama proses hidup.
inline jclass  g_cb_class  = nullptr;
inline jobject g_cb_object = nullptr;   // GlobalRef instance hooker
inline jobject g_backup    = nullptr;   // GlobalRef Method asli

// Muat callback DEX (SEAM #2) & kembalikan jclass (GlobalRef), atau nullptr
// (DEX absen / gagal) — fail-safe.
inline jclass load_callback_class(JNIEnv* env) {
#ifndef TT_HAVE_HOOK_DEX
    TT_LSP_LOGE("L3: callback DEX (tt_hook_dex.h) tak ada di build ini — hook dilewati");
    return nullptr;
#else
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
    jobject cls = env->CallObjectMethod(loader, loadClass, name);
    env->DeleteLocalRef(name);
    if (!cls || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

    jclass g = (jclass)env->NewGlobalRef(cls);
    env->DeleteLocalRef(cls);
    return g;
#endif
}

// Hook Settings.Secure.getString(ContentResolver, String) supaya key
// "android_id" mengembalikan `value`, key lain mengembalikan nilai asli.
inline bool hook_android_id(JNIEnv* env, const std::string& value) {
    if (!env) return false;
    set_android_id(value);

    // 1) Class callback dari DEX.
    if (!g_cb_class) g_cb_class = load_callback_class(env);
    if (!g_cb_class) return false;                 // fail-safe (DEX absen / gagal)

    // 2) Field statik `spoof` = android_id persona.
    jfieldID fSpoof = env->GetStaticFieldID(g_cb_class, "spoof", "Ljava/lang/String;");
    if (!fSpoof || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    jstring jval = env->NewStringUTF(value.c_str());
    env->SetStaticObjectField(g_cb_class, fSpoof, jval);
    env->DeleteLocalRef(jval);

    // 3) Instance hooker + reflected method `handle` (Object handle(Object[])).
    jmethodID hCtor = env->GetMethodID(g_cb_class, "<init>", "()V");
    if (!hCtor || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    jobject hooker = env->NewObject(g_cb_class, hCtor);
    if (!hooker || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    jmethodID hId = env->GetMethodID(g_cb_class, "handle",
        "([Ljava/lang/Object;)Ljava/lang/Object;");
    if (!hId || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    jobject cb = env->ToReflectedMethod(g_cb_class, hId, JNI_FALSE);
    if (!cb || env->ExceptionCheck()) { env->ExceptionClear(); return false; }

    // 4) Target: Settings$Secure.getString(ContentResolver, String) [STATIC].
    jclass sec = env->FindClass("android/provider/Settings$Secure");
    if (!sec || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    jmethodID mid = env->GetStaticMethodID(sec, "getString",
        "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;");
    if (!mid || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    jobject target = env->ToReflectedMethod(sec, mid, JNI_TRUE);
    if (!target || env->ExceptionCheck()) { env->ExceptionClear(); return false; }

    // 5) Pasang hook. backup = Method asli untuk chaining di sisi Java.
    jobject backup = lsplant::Hook(env, target, hooker, cb);
    if (!backup || env->ExceptionCheck()) { env->ExceptionClear(); return false; }

    // 6) Simpan backup ke field statik `original` supaya callback Java bisa
    //    memanggil nilai asli untuk key selain android_id.
    jfieldID fOrig = env->GetStaticFieldID(g_cb_class, "original", "Ljava/lang/reflect/Method;");
    if (!fOrig || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    env->SetStaticObjectField(g_cb_class, fOrig, backup);

    // 7) Tahan ref global supaya tak di-GC selama proses hidup.
    g_cb_object = env->NewGlobalRef(hooker);
    g_backup    = env->NewGlobalRef(backup);
    TT_LSP_LOGD("L3 Settings.Secure.getString hooked; android_id -> %s", value.c_str());
    return true;
}
#endif // TT_ENABLE_LSPLANT

} // namespace ttlsp
