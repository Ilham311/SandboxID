#pragma once

#include <cstdint>
#include <cstddef>
#include <unistd.h>
#include <cerrno>

namespace sandboxid {

// Single source of truth for the module id (== the install directory under
// /data/adb/modules). build.sh passes -DSBX_MODULE_ID="$(id from module.prop)";
// default keeps the historical "sandboxid" so a plain build is byte-identical.
// Renaming the module (the "New Identity" rebrand) is then one change in
// module.prop — every path below repoints automatically. NOTE: the disguised
// LSPlant callback classes (androidx.core.os.EnvCompatState / HandlerCompatRef)
// are deliberately NOT derived from this and must stay camouflaged.
#ifndef SBX_MODULE_ID
#define SBX_MODULE_ID "sandboxid"
#endif
#define SBX_MODDIR "/data/adb/modules/" SBX_MODULE_ID

inline constexpr char MODDIR[]        = SBX_MODDIR;
inline constexpr char IDENTITY_FILE[] = SBX_MODDIR "/identity.prop";
inline constexpr char MOUNTDIR[]      = SBX_MODDIR "/mount";
inline constexpr char TARGET_FILE[]   = SBX_MODDIR "/target.txt";

inline constexpr char PERSONAS_FILE[] = SBX_MODDIR "/personas.tsv";

inline constexpr char PERSONA_OVERRIDE[] = SBX_MODDIR "/persona.override";

inline constexpr char IDENTITY_BAK[]  = SBX_MODDIR "/identity.prop.bak";
inline constexpr char MODE_FILE[]     = SBX_MODDIR "/identity.mode";

inline constexpr char CARRIER_CONF[]  = SBX_MODDIR "/carrier.conf";
inline constexpr char CARRIERS_FILE[] = SBX_MODDIR "/carriers.tsv";
inline constexpr char RESETPROP[]     = SBX_MODDIR "/bin/resetprop-rs";

inline constexpr char ENABLE_HIDE[]   = SBX_MODDIR "/enable_hide";

enum Cmd : uint8_t {
    CMD_GET_IDENTITY = 2,
    CMD_DO_MOUNTS    = 3,
    CMD_DO_HIDE      = 4,
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
    {"GSM_CARRIER_ID",         "787"},
    {"GSM_SIM_STATE",          ""},
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
};
inline constexpr size_t VAL_DEFAULTS_N = sizeof(VAL_DEFAULTS) / sizeof(VAL_DEFAULTS[0]);

// Indonesian SIM/carrier pool (MCC 510) for native auto-rotation. main.cpp picks
// ONE entry per run from the persona seed when the blob didn't pin an operator
// (no manual carrier.conf), so gsm.operator.* (L2) and TelephonyManager getters
// (L3) stay coherent and rotate together on every action.sh run — no carriers.tsv
// needed for the default path. carrier_id "" => leave GSM_CARRIER_ID unset
// (framework treats it as UNKNOWN/-1). numeric = MCC(3)+MNC(2). Mirrors the
// 6 Indonesian rows in data/carriers.tsv (real MCC/MNC/ISO/carrier_id).
// carrier_id verified against AOSP carrier_list.textpb: Telkomsel 787, Indosat
// 789 ("Indosat - M3"), XL/Axis 788 (both map to the shared "XL/AXIS" entry).
// Tri (51089) and Smartfren (51009) are NOT in the resolvable slice, so their
// carrier_id is left "" (UNKNOWN) rather than fabricated — a wrong cid alongside
// a valid MCC/MNC is itself a tampering tell. L3 (sbx_lsplant.hpp) spoofs "" to
// TelephonyManager.UNKNOWN_CARRIER_ID (-1) instead of passing through to the
// real getter, so the real device's carrier_id is never leaked alongside the
// spoofed MCC/MNC.
struct SimCarrier { const char* alpha; const char* numeric; const char* iso; const char* carrier_id; };
inline constexpr SimCarrier ID_CARRIERS[] = {
    {"Telkomsel", "51010", "id", "787"},
    {"Indosat",   "51021", "id", "789"},
    {"XL",        "51011", "id", "788"},
    {"Axis",      "51008", "id", "788"},
    {"Tri",       "51089", "id", ""},
    {"Smartfren", "51009", "id", ""},
};
inline constexpr size_t ID_CARRIERS_N = sizeof(ID_CARRIERS) / sizeof(ID_CARRIERS[0]);

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

    {"ro.build.version.codename",       "REL"},
    {"ro.build.version.all_codenames",  "REL"},

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
