// jni/prop_map.hpp
// Property-key -> identity-key lookup tables used by hook_prop_get in main.cpp.
//
// v1.1.0 refactor: extracted from an 89-line hook_prop_get function that had two
// large inline map<> literals inside it. Splitting maps into this header keeps
// the hook body compact (~25 lines) and lets each table be unit-tested in
// isolation if wanted later.

#pragma once
#include <map>
#include <string>

namespace ternak_tt {

// System-property key -> identity.prop key. Value is looked up from the parsed
// identity blob (see main.cpp:parse_blob and g_id).
inline const std::map<std::string, std::string>& prop_to_identity_map() {
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
        {"ro.product.board",                "BOARD"},
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
        {"ro.build.characteristics",        "BUILD_CHARACTERISTICS"},
        {"persist.sys.timezone",            "PERSIST_TIMEZONE"},
        {"ro.product.cpu.abi",              "CPU_ABI"},
        {"ro.product.cpu.abi2",             "CPU_ABI2"},
        {"ro.product.cpu.abilist",          "CPU_ABILIST"},
        {"ro.product.cpu.abilist64",        "CPU_ABILIST64"},
        {"ro.product.cpu.abilist32",        "CPU_ABILIST32"},
        {"dalvik.vm.heapgrowthlimit",       "DALVIK_HEAPGROWTHLIMIT"},
        {"ro.mediacodec.min_sample_rate",   "MEDIACODEC_MIN_RATE"},
        {"ro.mediacodec.max_sample_rate",   "MEDIACODEC_MAX_RATE"},
        {"ro.build.user",                   "USER"},
        {"ro.build.host",                   "HOST"},
        {"ro.build.tags",                   "TAGS"},
        {"ro.build.type",                   "TYPE"},
    };
    return m;
}

// Static defaults returned directly (no identity lookup).
inline const std::map<std::string, std::string>& prop_static_defaults() {
    static const std::map<std::string, std::string> m = {
        {"gsm.operator.isroaming",          "false"},
        {"ro.zygote",                       "zygote64_32"},
        {"ro.hardware",                     "qcom"},
        {"ro.board.platform",               "sm8250"},
        {"ro.dalvik.vm.native.bridge",      "0"},
        {"ro.allow.mock.location",          "0"},
        {"dalvik.vm.isa.arm64.variant",     "generic"},
        {"dalvik.vm.isa.arm64.features",    "default"},
        {"dalvik.vm.isa.arm.variant",       "generic"},
        {"dalvik.vm.isa.arm.features",      "default"},
        {"dalvik.vm.heapsize",              "512m"},
        {"ro.build.version.preview_sdk",    "0"},
        {"persist.radio.multisim.config",   "ss"},
    };
    return m;
}

// Fallback identity values when a mapped identity key is absent from the blob.
inline const std::map<std::string, std::string>& identity_fallback_defaults() {
    static const std::map<std::string, std::string> m = {
        {"SYS_BOOT_COMPLETED",     "1"},
        {"GSM_OPERATOR_NUMERIC",   "51010"},
        {"GSM_OPERATOR_ALPHA",     "Telkomsel"},
        {"GSM_OPERATOR_ISO",       "id"},
        {"BUILD_CHARACTERISTICS",  "default"},
        {"PERSIST_TIMEZONE",       "Asia/Jakarta"},
        {"CPU_ABI",                "arm64-v8a"},
        {"CPU_ABI2",               ""},
        {"CPU_ABILIST",            "arm64-v8a,armeabi-v7a,armeabi"},
        {"CPU_ABILIST64",          "arm64-v8a"},
        {"CPU_ABILIST32",          "armeabi-v7a,armeabi"},
        {"DALVIK_HEAPGROWTHLIMIT", "256m"},
        {"MEDIACODEC_MIN_RATE",    "8000"},
        {"MEDIACODEC_MAX_RATE",    "192000"},
        {"DEBUG_FORCE_RTL",        "false"},
        {"MULTISIM_CONFIG",        ""},
    };
    return m;
}

} // namespace ternak_tt
