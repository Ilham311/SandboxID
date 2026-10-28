#include "module_impl.hpp"
#include "prop_defs.hpp"

const std::string& val(const std::string& k) {
    static const std::string empty;
    auto it = g_identity.find(k);
    if (it != g_identity.end() && !it->second.empty()) return it->second;
    static const std::map<std::string, std::string> defaults = [] {
        std::map<std::string, std::string> m;
        for (size_t i = 0; i < sandboxid::VAL_DEFAULTS_N; ++i)
            m.emplace(sandboxid::VAL_DEFAULTS[i].k, sandboxid::VAL_DEFAULTS[i].v);
        return m;
    }();
    auto d = defaults.find(k);
    if (d != defaults.end()) return d->second;
    return empty;
}

jstring (*orig_native_get)(JNIEnv*, jclass, jstring, jstring) = nullptr;

// Property-name -> identity-key map for the JAVA surface (in-app
// SystemProperties.native_get). It is generated from sandboxid::NATIVE_PROPS,
// the canonical prop-name -> identity-key table, so this map can never drift
// from it (previously this was a hand-maintained
// ~160-entry copy that was missing ro.product.bootimage.build.fingerprint,
// ro.product.vendor_dlkm.build.fingerprint and the *_for_attestation props while
// listing gsm.sim.operator.iso-country twice).
static const std::map<std::string, std::string>& prop_to_identity_map() {
    struct Extra { const char* k; const char* v; };
    // Props that are spoofed for in-app reads only: they describe the runtime
    // environment rather than the build, and are deliberately NOT pushed to the
    // native/mount surfaces (mounting an ABI or Dalvik override would break the
    // art runtime on the device).
    static const Extra extras[] = {
        {"ro.product.vendor.marketname",  "MARKETNAME"},
        {"ro.product.system.marketname",  "MARKETNAME"},
        {"ro.product.odm.marketname",     "MARKETNAME"},
        {"ro.product.product.marketname", "MARKETNAME"},
        {"ro.product.cpu.abi",            "CPU_ABI"},
        {"ro.product.cpu.abi2",           "CPU_ABI2"},
        {"ro.product.cpu.abilist",        "SUPPORTED_ABIS"},
        {"ro.product.cpu.abilist64",      "SUPPORTED_64_BIT_ABIS"},
        {"ro.product.cpu.abilist32",      "SUPPORTED_32_BIT_ABIS"},
        {"ro.board.platform",             "BOARD_PLATFORM"},
        {"ro.product.board",              "BOARD"},
        {"ro.hardware",                   "HARDWARE"},
        // AOSP's Build.BOOTIMAGE reads ro.bootimage.build.fingerprint; the
        // partition-namespaced ro.product.bootimage.build.fingerprint comes
        // from NATIVE_PROPS above. Both must resolve to FINGERPRINT.
        {"ro.bootimage.build.fingerprint", "FINGERPRINT"},
        {"ro.build.version.sdk",          "SDK_INT"},
        {"ro.build.version.preview_sdk",  "PREVIEW_SDK_INT"},
        {"sys.boot_completed",            "SYS_BOOT_COMPLETED"},
        {"debug.force_rtl",               "DEBUG_FORCE_RTL"},
        {"persist.radio.multisim.config", "MULTISIM_CONFIG"},
        {"gsm.sim.state.ril",             "GSM_SIM_STATE"},
        {"ro.build.characteristics",      "BUILD_CHARACTERISTICS"},
        {"persist.sys.timezone",          "PERSIST_TIMEZONE"},
        {"dalvik.vm.heapgrowthlimit",     "DALVIK_HEAPGROWTHLIMIT"},
        {"ro.mediacodec.min_sample_rate", "MEDIACODEC_MIN_RATE"},
        {"ro.mediacodec.max_sample_rate", "MEDIACODEC_MAX_RATE"},
    };

    static const std::map<std::string, std::string> m = [] {
        std::map<std::string, std::string> m;
        for (size_t i = 0; i < sandboxid::NATIVE_PROPS_N; ++i) {
            const char* idk = sandboxid::NATIVE_PROPS[i].identity_val;
            if (idk && idk[0]) m.emplace(sandboxid::NATIVE_PROPS[i].key, idk);
        }
        for (const Extra& e : extras) m.emplace(e.k, e.v);
        return m;
    }();
    return m;
}

// Fields whose value is derived from the SoC rather than stored. An identity
// written before the field existed (one already on disk) has no entry for it,
// so deriving here keeps the fix effective without forcing a re-roll - and it
// is the single place both the Java surface and the native prop surface go
// through, so the two can never disagree.
static bool derive_hw_value(const std::string& k, std::string& out) {
    const std::string& man = val("SOC_MANUFACTURER");
    const std::string& mod = val("SOC_MODEL");
    const std::string& plat = val("BOARD_PLATFORM");
    if (k == "DALVIK_ARM64_VARIANT")
        { out = sbxhw::dalvik_arm64_variant(man, mod, plat); return true; }
    if (k == "DALVIK_ARM_VARIANT")
        { out = sbxhw::dalvik_arm_variant(man, mod, plat); return true; }
    // GPU_DRIVER is deliberately NOT derived. ro.gfx.driver.1 names an
    // installed package, and GraphicsEnvironment.get() checks that with
    // PackageManager: a derived name (com.qualcomm.qti.gpudrivers.<platform>.
    // api30) does not exist on the device, so the framework logs "updatable
    // driver package '<name>' not installed" - a louder tell than the leak.
    // The whole ro.gfx.driver.* family is hidden by is_vendor_rom_prop().
    return false;
}

bool spoof_prop_value(const std::string& k, std::string& out) {
    const auto& map = prop_to_identity_map();
    auto it = map.find(k);
    if (it != map.end()) {
        const std::string& v = val(it->second);
        if (!v.empty()) { out = v; return true; }
        if (derive_hw_value(it->second, out)) return true;
    }
    for (size_t i = 0; i < sandboxid::STATIC_PROP_DEFAULTS_N; ++i) {
        if (k == sandboxid::STATIC_PROP_DEFAULTS[i].k) {
            out = sandboxid::STATIC_PROP_DEFAULTS[i].v;
            return true;
        }
    }
    return false;
}

static jstring hook_prop_get(JNIEnv* env, jclass clazz, jstring j_key, jstring j_def) {
    if (!j_key) return j_def;
    JniString raw(env, j_key);
    if (!raw) { if (env->ExceptionCheck()) env->ExceptionClear(); return j_def; }
    std::string k(raw.c_str());
    LOGD("PROP native_get('%s')", k.c_str());
    std::string v;
    if (spoof_prop_value(k, v)) {
        LOGD("PROP SPOOF '%s' -> '%s'", k.c_str(), v.c_str());
        return env->NewStringUTF(v.c_str());
    }
    if (sbx_prop_hidden(k.c_str())) {
        LOGD("PROP HIDE '%s' (report absent)", k.c_str());
        return j_def;
    }
    if (orig_native_get) return orig_native_get(env, clazz, j_key, j_def);
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(k.c_str(), buf) > 0) return env->NewStringUTF(buf);
    return j_def;
}

void install_prop_hook(Api* api, JNIEnv* env) {
    JNINativeMethod m = {
        const_cast<char*>("native_get"),
        const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
        reinterpret_cast<void*>(hook_prop_get),
    };
    api->hookJniNativeMethods(env, "android/os/SystemProperties", &m, 1);
    if (env->ExceptionCheck())
        env->ExceptionClear();
    orig_native_get = reinterpret_cast<jstring (*)(JNIEnv*, jclass, jstring, jstring)>(m.fnPtr);
    if (!orig_native_get)
        LOGE("PROP: native_get hook FAILED (fnPtr null)");
    else
        LOGD("PROP native_get hooked (orig=%p)", reinterpret_cast<void*>(orig_native_get));
}