// tt_paths.hpp — Shared path constants across companion / module / CLI.
//
// NEW in patch v2.1 (P3 refactor): removes triplicate string literals from
// main.cpp / companion.cpp / ternak-tt.cpp so drift bugs like the previous
// BIND_ENTRIES mismatch cannot happen again.
//
// Header-only, constexpr-only. Safe to include from anywhere in the module.

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

// Canonical bind-entry table. Companion uses this to overlay build.prop
// files into the target process's mount namespace. The XML overlay is
// enumerated dynamically per-user (see improvements/multiuser_paths.hpp).
struct BindEntry {
    const char* src_rel;  // relative to MOUNTDIR
    const char* dst;      // absolute destination path
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

// Zygisk companion IPC command IDs.
// Note: v2.0 used CMD_CHECK_TT = 1 but never handled it -> removed.
enum : unsigned char {
    CMD_GET_IDENTITY = 2,
    CMD_DO_MOUNTS    = 3,
};

} // namespace paths
} // namespace tt
