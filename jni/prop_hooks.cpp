#include "module_impl.hpp"

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
static const std::map<std::string, std::string>& prop_to_identity_map() {
    static const std::map<std::string, std::string> m = {
        {"ro.serialno",                     "SERIAL"},
        {"ro.boot.serialno",                "SERIAL"},
        {"ro.build.fingerprint",            "FINGERPRINT"},
        {"ro.bootimage.build.fingerprint",  "FINGERPRINT"},
        {"ro.product.model",                "MODEL"},
        {"ro.product.brand",                "BRAND"},
        {"ro.product.manufacturer",         "MANUFACTURER"},
        {"ro.product.device",               "DEVICE"},
        {"ro.product.name",                 "PRODUCT"},
        {"ro.product.marketname",           "MARKETNAME"},
        {"ro.product.vendor.marketname",    "MARKETNAME"},
        {"ro.product.system.marketname",    "MARKETNAME"},
        {"ro.product.odm.marketname",       "MARKETNAME"},
        {"ro.product.product.marketname",   "MARKETNAME"},
        {"ro.product.board",                "BOARD"},
        {"ro.hardware",                     "HARDWARE"},
        {"ro.board.platform",               "BOARD_PLATFORM"},
        {"ro.soc.manufacturer",             "SOC_MANUFACTURER"},
        {"ro.soc.model",                    "SOC_MODEL"},
        {"ro.build.id",                     "ID"},
        {"ro.build.display.id",             "DISPLAY"},
        {"ro.build.description",            "DESCRIPTION"},
        {"ro.build.version.release",        "RELEASE"},
        {"ro.build.version.sdk",            "SDK_INT"},
        {"ro.build.version.security_patch", "SECURITY_PATCH"},
        {"ro.build.version.incremental",    "INCREMENTAL"},
        {"gsm.version.baseband",            "RADIO"},
        {"sys.boot_completed",              "SYS_BOOT_COMPLETED"},
        {"debug.force_rtl",                 "DEBUG_FORCE_RTL"},
        {"persist.radio.multisim.config",   "MULTISIM_CONFIG"},
        {"gsm.operator.numeric",            "GSM_OPERATOR_NUMERIC"},
        {"gsm.sim.operator.numeric",        "GSM_OPERATOR_NUMERIC"},
        {"gsm.operator.alpha",              "GSM_OPERATOR_ALPHA"},
        {"gsm.sim.operator.alpha",          "GSM_OPERATOR_ALPHA"},
        {"gsm.operator.iso-country",        "GSM_OPERATOR_ISO"},
        {"gsm.sim.operator.iso-country",    "GSM_OPERATOR_ISO"},
        {"gsm.sim.state",                   "GSM_SIM_STATE"},
        {"gsm.sim.state.ril",               "GSM_SIM_STATE"},
        {"ro.build.characteristics",        "BUILD_CHARACTERISTICS"},
        {"persist.sys.timezone",            "PERSIST_TIMEZONE"},
        {"ro.product.cpu.abi",              "CPU_ABI"},
        {"ro.product.cpu.abi2",             "CPU_ABI2"},
        {"ro.product.cpu.abilist",          "SUPPORTED_ABIS"},
        {"ro.product.cpu.abilist64",        "SUPPORTED_64_BIT_ABIS"},
        {"ro.product.cpu.abilist32",        "SUPPORTED_32_BIT_ABIS"},
        {"dalvik.vm.heapgrowthlimit",       "DALVIK_HEAPGROWTHLIMIT"},
        {"ro.mediacodec.min_sample_rate",   "MEDIACODEC_MIN_RATE"},
        {"ro.mediacodec.max_sample_rate",   "MEDIACODEC_MAX_RATE"},
        {"ro.build.user",                   "USER"},
        {"ro.build.host",                   "HOST"},
        {"ro.build.tags",                   "TAGS"},
        {"ro.build.type",                   "TYPE"},
        {"ro.build.date.utc",               "BUILD_TIME_UTC"},
        {"ro.build.date",                   "BUILD_DATE"},
        {"ro.build.flavor",                 "FLAVOR"},
        {"ro.boot.vbmeta.digest",           "VBMETA_DIGEST"},
        {"ro.build.product",                     "DEVICE"},
        {"ro.build.version.release_or_codename", "RELEASE"},
        {"ro.vendor.build.security_patch",       "SECURITY_PATCH"},
        {"ro.system.build.fingerprint",     "FINGERPRINT"},
        {"ro.vendor.build.fingerprint",     "FINGERPRINT"},
        {"ro.odm.build.fingerprint",        "FINGERPRINT"},
        {"ro.product.build.fingerprint",    "FINGERPRINT"},
        {"ro.system_ext.build.fingerprint", "FINGERPRINT"},
        {"ro.vendor_dlkm.build.fingerprint", "FINGERPRINT"},
        {"ro.odm_dlkm.build.fingerprint",   "FINGERPRINT"},
        {"ro.product.system.model",         "MODEL"},
        {"ro.product.vendor.model",         "MODEL"},
        {"ro.product.odm.model",            "MODEL"},
        {"ro.product.product.model",        "MODEL"},
        {"ro.product.system_ext.model",     "MODEL"},
        {"ro.product.system.brand",         "BRAND"},
        {"ro.product.vendor.brand",         "BRAND"},
        {"ro.product.odm.brand",            "BRAND"},
        {"ro.product.product.brand",        "BRAND"},
        {"ro.product.system_ext.brand",     "BRAND"},
        {"ro.product.system.manufacturer",     "MANUFACTURER"},
        {"ro.product.vendor.manufacturer",     "MANUFACTURER"},
        {"ro.product.odm.manufacturer",        "MANUFACTURER"},
        {"ro.product.product.manufacturer",    "MANUFACTURER"},
        {"ro.product.system_ext.manufacturer", "MANUFACTURER"},
        {"ro.product.system.device",        "DEVICE"},
        {"ro.product.vendor.device",        "DEVICE"},
        {"ro.product.odm.device",           "DEVICE"},
        {"ro.product.product.device",       "DEVICE"},
        {"ro.product.system_ext.device",    "DEVICE"},
        {"ro.product.system.name",          "PRODUCT"},
        {"ro.product.vendor.name",          "PRODUCT"},
        {"ro.product.odm.name",             "PRODUCT"},
        {"ro.product.product.name",         "PRODUCT"},
        {"ro.product.system_ext.name",      "PRODUCT"},
        {"ro.boot.hardware.sku",                     "SKU"},
        {"ro.boot.product.hardware.sku",             "ODM_SKU"},
        {"ro.build.version.base_os",                 "BASE_OS"},
        {"ro.build.version.preview_sdk",             "PREVIEW_SDK_INT"},
        {"ro.build.version.preview_sdk_fingerprint", "PREVIEW_SDK_FINGERPRINT"},
        {"ro.odm.build.media_performance_class",     "MEDIA_PERFORMANCE_CLASS"},
        // New additions from DeviceSpoofLab/DeviceAnon research
        {"ro.kernel.qemu",                   "QEMU_0"},
        {"ro.boot.qemu",                    "QEMU_0"},
        {"ro.adb.secure",                   "ADB_SECURE_1"},
        {"ro.boot.warranty_bit",            "WARRANTY_0"},
        {"ro.crypto.state",                 "CRYPTO_ENCRYPTED"},
        {"ro.treble.enabled",               "TREBLE_TRUE"},
        {"ro.boot.mode",                    "BOOT_MODE_NORMAL"},
        {"ro.arch",                         "ARCH_ARM64"},
        {"ro.boot.hardware",                "HARDWARE"},
        {"ro.build.brand",                  "BRAND"},
        {"ro.build.manufacturer",           "MANUFACTURER"},
        {"ro.build.device",                 "DEVICE"},
        {"ro.sf.lcd_density",               "LCD_DENSITY"},
        {"ro.product.vendor_dlkm.model",    "MODEL"},
        {"ro.product.bootimage.model",      "MODEL"},
        {"ro.product.system_dlkm.model",    "MODEL"},
        {"gsm.sim.operator.iso-country",    "GSM_OPERATOR_ISO"},
        {"ro.system_dlkm.build.fingerprint", "FINGERPRINT"},
        {"ro.vendor_dlkm.build.version.release", "RELEASE"},
        {"ro.odm.build.version.release",    "RELEASE"},
        {"ro.bootimage.build.version.release", "RELEASE"},
        {"ro.system_dlkm.build.version.release", "RELEASE"},
        {"ro.vendor.build.version.release_or_codename", "RELEASE"},
        {"ro.vendor_dlkm.build.version.release_or_codename", "RELEASE"},
        {"ro.odm.build.version.release_or_codename", "RELEASE"},
        {"ro.bootimage.build.version.release_or_codename", "RELEASE"},
        {"ro.system_dlkm.build.version.release_or_codename", "RELEASE"},
        {"ro.product.build.version.sdk",    "SDK_INT"},
        {"ro.product.build.version.release", "RELEASE"},
        {"ro.product.build.id",             "ID"},
        {"ro.product.build.tags",           "TAGS"},
        {"ro.product.build.type",           "TYPE"},
        {"ro.product.build.version.incremental", "INCREMENTAL"},
        {"ro.product.vendor_dlkm.brand",    "BRAND"},
        {"ro.product.bootimage.brand",      "BRAND"},
        {"ro.product.system_dlkm.brand",    "BRAND"},
        {"ro.product.vendor_dlkm.device",   "DEVICE"},
        {"ro.product.bootimage.device",     "DEVICE"},
        {"ro.product.system_dlkm.device",   "DEVICE"},
        {"ro.product.vendor_dlkm.name",     "PRODUCT"},
        {"ro.product.bootimage.name",       "PRODUCT"},
        {"ro.product.system_dlkm.name",     "PRODUCT"},
        {"ro.product.vendor_dlkm.manufacturer", "MANUFACTURER"},
        {"ro.product.bootimage.manufacturer",   "MANUFACTURER"},
        {"ro.product.system_dlkm.manufacturer", "MANUFACTURER"},
    };
    return m;
}

bool spoof_prop_value(const std::string& k, std::string& out) {
    const auto& map = prop_to_identity_map();
    auto it = map.find(k);
    if (it != map.end()) {
        const std::string& v = val(it->second);
        if (!v.empty()) { out = v; return true; }
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