#pragma once

#include <cstdint>
#include <cstddef>
#include <unistd.h>
#include <cerrno>

namespace sandboxid {

inline constexpr char MODDIR[]        = "/data/adb/modules/sandboxid";
inline constexpr char IDENTITY_FILE[] = "/data/adb/modules/sandboxid/identity.prop";
inline constexpr char TARGET_FILE[]   = "/data/adb/modules/sandboxid/target.txt";

inline constexpr char PERSONAS_FILE[] = "/data/adb/modules/sandboxid/personas.tsv";

inline constexpr char PERSONA_OVERRIDE[] = "/data/adb/modules/sandboxid/persona.override";

inline constexpr char IDENTITY_BAK[]  = "/data/adb/modules/sandboxid/identity.prop.bak";
inline constexpr char MODE_FILE[]     = "/data/adb/modules/sandboxid/identity.mode";

inline constexpr char CARRIER_CONF[]  = "/data/adb/modules/sandboxid/carrier.conf";

inline constexpr char ENABLE_HIDE[]   = "/data/adb/modules/sandboxid/enable_hide";

enum Cmd : uint8_t {
    CMD_GET_IDENTITY = 2,
    CMD_DO_HIDE      = 4,
};

inline constexpr uint32_t MAX_IDENTITY_BLOB = 64u * 1024u;

struct KV { const char* k; const char* v; };

inline constexpr KV VAL_DEFAULTS[] = {
    {"SYS_BOOT_COMPLETED",     "1"},
    // GSM_OPERATOR_* deliberately absent. They used to default to Telkomsel
    // 51010 / id / carrier id 787, which made the module claim a carrier the
    // device is not on: the test SIM is XL 51011, and the contradiction showed
    // up in Grab's own telemetry as `"mnc":"11"` (real, from a radio surface the
    // module does not hook) beside `"crn":"Telkomsel"` (spoofed, from the
    // property and TelephonyManager lenses) in the same event stream. A persona
    // that invents a carrier also breaks anything carrier-mediated - SNA
    // verification failed 28 times with "network not validated" - because the
    // network the modem is actually attached to is not the one being reported.
    //
    // No default is the correct default: with the key absent, val() returns
    // empty, spoof_prop_value() declines, and the REAL property is served. The
    // device then reports its real carrier consistently through every surface,
    // hooked or not. A user who wants a different carrier runs `carrier`, which
    // writes GSM_OPERATOR_* into identity.prop and is honoured everywhere the
    // module does hook (see README "Known limits" for the surfaces it cannot).
    {"BUILD_CHARACTERISTICS",  "default"},
    {"PERSIST_TIMEZONE",       "Asia/Jakarta"},
    {"CPU_ABI",                "arm64-v8a"},
    {"CPU_ABI2",               ""},
    {"SUPPORTED_ABIS",         "arm64-v8a,armeabi-v7a,armeabi"},
    {"SUPPORTED_64_BIT_ABIS",  "arm64-v8a"},
    {"SUPPORTED_32_BIT_ABIS",  "armeabi-v7a,armeabi"},
    {"DALVIK_HEAPGROWTHLIMIT", "256m"},
    {"MEDIACODEC_MIN_RATE",    "8000"},
    {"MEDIACODEC_MAX_RATE",    "192000"},
    {"DEBUG_FORCE_RTL",        "false"},
    {"MULTISIM_CONFIG",        ""},
    {"LCD_DENSITY",            "420"},
    {"QEMU_0",                 "0"},
    {"ADB_SECURE_1",           "1"},
    {"WARRANTY_0",             "0"},
    {"CRYPTO_ENCRYPTED",       "encrypted"},
    {"TREBLE_TRUE",            "true"},
    {"BOOT_MODE_NORMAL",       "normal"},
    {"ARCH_ARM64",             "arm64"},
};
inline constexpr size_t VAL_DEFAULTS_N = sizeof(VAL_DEFAULTS) / sizeof(VAL_DEFAULTS[0]);

inline constexpr KV STATIC_PROP_DEFAULTS[] = {
    {"gsm.operator.isroaming",       "false"},
    {"ro.zygote",                    "zygote64_32"},
    {"ro.dalvik.vm.native.bridge",   "0"},
    {"ro.allow.mock.location",       "0"},
    // dalvik.vm.isa.*.variant is NOT here any more. "generic" is the
    // AOSP/GSI/emulator default, not what a retail device ships, and serving it
    // for every persona was a contradiction for any device claiming to be a
    // modern flagship. The variant is derived per-persona from the SoC in
    // spoof_prop_value() (see hw_naming.hpp), which also covers an identity
    // written before that field existed. The .features halves stay: "default"
    // is what real devices report.
    {"dalvik.vm.isa.arm64.features", "default"},
    {"dalvik.vm.isa.arm.features",   "default"},
    {"dalvik.vm.heapsize",           "512m"},
    {"ro.build.version.preview_sdk", "0"},
    {"ro.build.version.preview_sdk_fingerprint", "REL"},
    // Constant-valued props a retail device advertises; previously spoofs via
    // the identity map, now served from here so the identity map can be
    // generated from NATIVE_PROPS without losing them.
    {"ro.arch",                        "arm64"},
    {"ro.boot.mode",                   "normal"},
    {"ro.boot.qemu",                   "0"},
    {"ro.kernel.qemu",                 "0"},
    {"ro.boot.warranty_bit",           "0"},
    {"ro.adb.secure",                  "1"},
    {"ro.crypto.state",                "encrypted"},
    {"ro.treble.enabled",              "true"},

    {"ro.build.version.codename",       "REL"},
    {"ro.build.version.all_codenames",  "REL"},

    {"ro.bootloader",                   "unknown"},
    {"ro.boot.bootloader",              "unknown"},
    {"ro.boot.verifiedbootstate",       "green"},
    {"ro.boot.vbmeta.device_state",     "locked"},
    {"ro.boot.flash.locked",            "1"},
    {"ro.boot.veritymode",              "enforcing"},
    {"ro.boot.vbmeta.hash_alg",         "sha256"},
    {"ro.boot.vbmeta.avb_version",      "1.0"},
    {"ro.boot.vbmeta.invalidate_on_error", "yes"},
    {"ro.secure",                       "1"},
    {"ro.debuggable",                   "0"},
    {"ro.build.selinux",                "1"},
    {"sys.oem_unlock_allowed",          "0"},
    {"persist.sys.usb.config",          "none"},
};
inline constexpr size_t STATIC_PROP_DEFAULTS_N =
    sizeof(STATIC_PROP_DEFAULTS) / sizeof(STATIC_PROP_DEFAULTS[0]);

inline bool read_full(int fd, void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, p + got, n - got);
        if (r > 0) { got += static_cast<size_t>(r); continue; }
        if (r < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

inline bool write_full(int fd, const void* buf, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t put = 0;
    while (put < n) {
        ssize_t w = ::write(fd, p + put, n - put);
        if (w > 0) { put += static_cast<size_t>(w); continue; }
        if (w < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

}