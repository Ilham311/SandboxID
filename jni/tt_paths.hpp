

#pragma once

#include <cstddef>

namespace tt {
namespace paths {

inline constexpr const char* MODDIR        = "/data/adb/modules/ternak_tt";
inline constexpr const char* IDENTITY_FILE = "/data/adb/modules/ternak_tt/identity.prop";
inline constexpr const char* IDENTITY_BAK  = "/data/adb/modules/ternak_tt/identity.prop.bak";
inline constexpr const char* MODE_FILE     = "/data/adb/modules/ternak_tt/identity.mode";
inline constexpr const char* RESETPROP     = "/data/adb/modules/ternak_tt/bin/resetprop-rs";
inline constexpr const char* MOUNTDIR      = "/data/adb/modules/ternak_tt/mount";
inline constexpr const char* TARGET_FILE   = "/data/adb/modules/ternak_tt/target.txt";
inline constexpr const char* PROPS_BATCH   = "/data/adb/modules/ternak_tt/mount/props.txt";

struct BindEntry {
    const char* src_rel;
    const char* dst;
};

inline constexpr BindEntry BUILD_PROP_ENTRIES[] = {
    {"system/build.prop",     "/system/build.prop"},
    {"vendor/build.prop",     "/vendor/build.prop"},
    {"odm/build.prop",        "/odm/etc/build.prop"},
    {"odm/build.prop",        "/odm/build.prop"},
    {"product/build.prop",    "/product/etc/build.prop"},
    {"product/build.prop",    "/product/build.prop"},
    {"system_ext/build.prop", "/system_ext/etc/build.prop"},
    {"system_ext/build.prop", "/system_ext/build.prop"},
};

inline constexpr std::size_t BUILD_PROP_ENTRIES_N =
    sizeof(BUILD_PROP_ENTRIES) / sizeof(BUILD_PROP_ENTRIES[0]);

enum : unsigned char {
    CMD_GET_IDENTITY = 2,
    CMD_DO_MOUNTS    = 3,
};

}
}
