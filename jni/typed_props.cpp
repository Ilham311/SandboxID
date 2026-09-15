#include "module_impl.hpp"

jint     (*orig_get_int)(JNIEnv*, jclass, jstring, jint)     = nullptr;
jlong    (*orig_get_long)(JNIEnv*, jclass, jstring, jlong)   = nullptr;
jboolean (*orig_get_bool)(JNIEnv*, jclass, jstring, jboolean)= nullptr;

static const std::map<std::string, jboolean>& sbx_bool_spoof() {
    static const std::map<std::string, jboolean> m = {
        {"sys.boot_completed",                       JNI_TRUE},
        {"debug.force_rtl",                          JNI_FALSE},
        {"framework.pause_bg_animations.enabled",    JNI_FALSE},
        {"dalvik.vm.dexopt.secondary",               JNI_TRUE},
        {"viewroot.profile_rendering",               JNI_FALSE},
        {"debug.sqlite.no_double_quoted_strs",       JNI_TRUE},
        {"persist.sys.activity_anim_perf_override",  JNI_FALSE},
        {"persist.sys.lmk.reportkills",              JNI_FALSE},
        {"debug.layout",                             JNI_FALSE},
    };
    return m;
}
static const std::map<std::string, jint>& sbx_int_spoof() {
    static const std::map<std::string, jint> m = {
        {"ro.mediacodec.min_sample_rate",         8000},
        {"ro.mediacodec.max_sample_rate",         192000},
        {"debug.sqlite.wal.autocheckpoint",       100},
        {"debug.sqlite.pagesize",                 4096},
        {"debug.sqlite.journalsizelimit",         524288},
        {"debug.sqlite.wal.truncatesize",         1048576},
        {"debug.sqlite.wal.poolsize",             0},
        {"debug.hwui.fps_divisor",                1},
        {"persist.wm.debug.ext_version_override", 0},
        {"build.version.extensions.r",            3},
        {"build.version.extensions.s",            4},
        {"build.version.extensions.t",            4},
        {"build.version.extensions.u",            13},
        {"build.version.extensions.v",            13},
        {"build.version.extensions.ad_services",  15},
        {"debug.am.run_gc_trim_level",            2147483647},
        {"debug.am.run_mallopt_trim_level",       2147483647},
        {"debug.adservices.binder_timeout",       10000},
        {"ro.build.version.preview_sdk",          0},
    };
    return m;
}
static const std::map<std::string, jlong>& sbx_long_spoof() {
    static const std::map<std::string, jlong> m = {
        {"ro.gfx.driver_build_time",              1704067200LL},
    };
    return m;
}

static jint hook_prop_get_int(JNIEnv* env, jclass clazz, jstring j_key, jint def) {
    if (!j_key) return def;
    const char* r = env->GetStringUTFChars(j_key, nullptr);
    if (!r) { if (env->ExceptionCheck()) env->ExceptionClear(); return def; }
    std::string k(r);
    env->ReleaseStringUTFChars(j_key, r);
    std::string v;
    if (spoof_prop_value(k, v)) {
        long long n = 0;
        if (sbx_parse_longlong(v, n)) { LOGD("TYPED SPI(id) '%s' -> %d", k.c_str(), (int)n); return (jint)n; }
    }
    const auto& m = sbx_int_spoof();
    auto it = m.find(k);
    if (it != m.end()) { LOGD("TYPED SPI '%s' -> %d", k.c_str(), it->second); return it->second; }
    if (sbx_should_suppress_key(k)) { LOGD("TYPED SPI SUPPRESS '%s'", k.c_str()); return def; }
    return orig_get_int ? orig_get_int(env, clazz, j_key, def) : def;
}
static jlong hook_prop_get_long(JNIEnv* env, jclass clazz, jstring j_key, jlong def) {
    if (!j_key) return def;
    const char* r = env->GetStringUTFChars(j_key, nullptr);
    if (!r) { if (env->ExceptionCheck()) env->ExceptionClear(); return def; }
    std::string k(r);
    env->ReleaseStringUTFChars(j_key, r);
    std::string v;
    if (spoof_prop_value(k, v)) {
        long long n = 0;
        if (sbx_parse_longlong(v, n)) { LOGD("TYPED SPL(id) '%s' -> %lld", k.c_str(), (long long)n); return (jlong)n; }
    }
    const auto& m = sbx_long_spoof();
    auto it = m.find(k);
    if (it != m.end()) { LOGD("TYPED SPL '%s' -> %lld", k.c_str(), (long long)it->second); return it->second; }
    if (sbx_should_suppress_key(k)) { LOGD("TYPED SPL SUPPRESS '%s'", k.c_str()); return def; }
    return orig_get_long ? orig_get_long(env, clazz, j_key, def) : def;
}
static jboolean hook_prop_get_bool(JNIEnv* env, jclass clazz, jstring j_key, jboolean def) {
    if (!j_key) return def;
    const char* r = env->GetStringUTFChars(j_key, nullptr);
    if (!r) { if (env->ExceptionCheck()) env->ExceptionClear(); return def; }
    std::string k(r);
    env->ReleaseStringUTFChars(j_key, r);
    const auto& m = sbx_bool_spoof();
    auto it = m.find(k);
    if (it != m.end()) { LOGD("TYPED SPB '%s' -> %d", k.c_str(), (int)it->second); return it->second; }
    if (sbx_should_suppress_key(k)) { LOGD("TYPED SPB SUPPRESS '%s'", k.c_str()); return def; }
    return orig_get_bool ? orig_get_bool(env, clazz, j_key, def) : def;
}

void install_leak_sensors(Api* api, JNIEnv* env) {
    JNINativeMethod m[3] = {
        {const_cast<char*>("native_get_int"),
         const_cast<char*>("(Ljava/lang/String;I)I"),
         reinterpret_cast<void*>(hook_prop_get_int)},
        {const_cast<char*>("native_get_long"),
         const_cast<char*>("(Ljava/lang/String;J)J"),
         reinterpret_cast<void*>(hook_prop_get_long)},
        {const_cast<char*>("native_get_boolean"),
         const_cast<char*>("(Ljava/lang/String;Z)Z"),
         reinterpret_cast<void*>(hook_prop_get_bool)},
    };
    api->hookJniNativeMethods(env, "android/os/SystemProperties", m, 3);
    if (env->ExceptionCheck())
        env->ExceptionClear();
    if (!m[0].fnPtr || !m[1].fnPtr || !m[2].fnPtr)
        LOGE("TYPED: leak-sensor hooks FAILED (fnPtr null: %d/%d/%d) — typed getters unspoofed",
             m[0].fnPtr ? 1 : 0, m[1].fnPtr ? 1 : 0, m[2].fnPtr ? 1 : 0);
    orig_get_int  = reinterpret_cast<jint (*)(JNIEnv*, jclass, jstring, jint)>(m[0].fnPtr);
    orig_get_long = reinterpret_cast<jlong (*)(JNIEnv*, jclass, jstring, jlong)>(m[1].fnPtr);
    orig_get_bool = reinterpret_cast<jboolean (*)(JNIEnv*, jclass, jstring, jboolean)>(m[2].fnPtr);
    LOGD("TYPED leak sensors installed (int/long/bool)");
}