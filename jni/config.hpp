#pragma once

#include <cstdint>
#include <cstddef>
#include <unistd.h>
#include <cerrno>

namespace sandboxid {

inline constexpr char MODDIR[]        = "/data/adb/modules/sandboxid";
inline constexpr char IDENTITY_FILE[] = "/data/adb/modules/sandboxid/identity.prop";
inline constexpr char MOUNTDIR[]      = "/data/adb/modules/sandboxid/mount";
inline constexpr char TARGET_FILE[]   = "/data/adb/modules/sandboxid/target.txt";
// Persona pool source (replaces the old compiled-in pool.hpp). Tab-separated,
// one Pixel persona per line; used as the offline fallback pool. See personas.tsv.
inline constexpr char PERSONAS_FILE[] = "/data/adb/modules/sandboxid/personas.tsv";

// One-shot persona override written by autopif.sh: a SINGLE tab-separated
// persona line (same 10-column format as personas.tsv). When present, `freshen`
// derives the identity from THIS persona directly -- bypassing the SDK-matched
// random pool pick -- then deletes the file, so every `action`/autopif run
// applies a fresh, randomly-fetched Pixel identity. Absent/invalid => normal
// pool pick. This is why autopif no longer needs to persist into personas.tsv.
inline constexpr char PERSONA_OVERRIDE[] = "/data/adb/modules/sandboxid/persona.override";

inline constexpr char IDENTITY_BAK[]  = "/data/adb/modules/sandboxid/identity.prop.bak";
inline constexpr char MODE_FILE[]     = "/data/adb/modules/sandboxid/identity.mode";
inline constexpr char RESETPROP[]     = "/data/adb/modules/sandboxid/bin/resetprop-rs";

enum Cmd : uint8_t {
    CMD_GET_IDENTITY = 2,
    CMD_DO_MOUNTS    = 3,
};

inline constexpr uint32_t MAX_IDENTITY_BLOB = 64u * 1024u;

struct BindEntry { const char* src_rel; const char* dst; };

inline constexpr BindEntry BIND_ENTRIES[] = {
    {"system/build.prop",     "/system/build.prop"},
    {"vendor/build.prop",     "/vendor/build.prop"},
    {"odm/build.prop",        "/odm/etc/build.prop"},
    {"odm/build.prop",        "/odm/build.prop"},
    {"product/build.prop",    "/product/etc/build.prop"},
    {"product/build.prop",    "/product/build.prop"},
    {"system_ext/build.prop", "/system_ext/etc/build.prop"},
    {"system_ext/build.prop", "/system_ext/build.prop"},

    {"settings_secure.xml",   "/data/system/users/0/settings_secure.xml"},
};
inline constexpr size_t BIND_ENTRIES_N = sizeof(BIND_ENTRIES) / sizeof(BIND_ENTRIES[0]);

inline constexpr const char* MOUNT_PARTS[] = {"system", "vendor", "odm", "product", "system_ext"};
inline constexpr size_t MOUNT_PARTS_N = sizeof(MOUNT_PARTS) / sizeof(MOUNT_PARTS[0]);

struct KV { const char* k; const char* v; };

inline constexpr KV VAL_DEFAULTS[] = {
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
inline constexpr size_t VAL_DEFAULTS_N = sizeof(VAL_DEFAULTS) / sizeof(VAL_DEFAULTS[0]);

inline constexpr KV STATIC_PROP_DEFAULTS[] = {
    {"gsm.operator.isroaming",       "false"},
    {"ro.zygote",                    "zygote64_32"},
    {"ro.dalvik.vm.native.bridge",   "0"},
    {"ro.allow.mock.location",       "0"},
    {"dalvik.vm.isa.arm64.variant",  "generic"},
    {"dalvik.vm.isa.arm64.features", "default"},
    {"dalvik.vm.isa.arm.variant",    "generic"},
    {"dalvik.vm.isa.arm.features",   "default"},
    {"dalvik.vm.heapsize",           "512m"},
    {"ro.build.version.preview_sdk", "0"},
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
