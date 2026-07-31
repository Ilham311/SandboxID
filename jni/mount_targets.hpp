// jni/mount_targets.hpp
// Shared mount/bind constants used by both the Zygisk module (main.cpp)
// and the companion process (companion.cpp).
//
// v1.1.0 refactor: extracted from DUPLICATE definitions in v1.0.23
//   - main.cpp had a 6-entry BIND_ENTRIES list
//   - companion.cpp had a 9-entry BIND_ENTRIES list
// These are now unified as the union (9 entries) below to ensure the module
// binds /odm/etc/, /product/etc/, and /system_ext/etc/ on Android 10+ ROMs
// where those paths carry the effective build.prop.

#pragma once
#include <cstddef>

namespace ternak_tt {

constexpr const char* MOUNTDIR      = "/data/adb/modules/ternak_tt/mount";
constexpr const char* IDENTITY_FILE = "/data/adb/modules/ternak_tt/identity.prop";
constexpr const char* TARGET_FILE   = "/data/adb/modules/ternak_tt/target.txt";

struct BindEntry {
    const char* src_rel; // path relative to MOUNTDIR (e.g. "system/build.prop")
    const char* dst;     // absolute destination bind target
};

constexpr BindEntry BIND_ENTRIES[] = {
    {"system/build.prop",       "/system/build.prop"},
    {"vendor/build.prop",       "/vendor/build.prop"},
    {"odm/build.prop",          "/odm/etc/build.prop"},
    {"odm/build.prop",          "/odm/build.prop"},
    {"product/build.prop",      "/product/etc/build.prop"},
    {"product/build.prop",      "/product/build.prop"},
    {"system_ext/build.prop",   "/system_ext/etc/build.prop"},
    {"system_ext/build.prop",   "/system_ext/build.prop"},
    {"settings_secure.xml",     "/data/system/users/0/settings_secure.xml"},
};

constexpr size_t BIND_ENTRIES_COUNT = sizeof(BIND_ENTRIES) / sizeof(BIND_ENTRIES[0]);

constexpr const char* DEFAULT_TARGETS[] = {
    "com.zhiliaoapp.musically",
    "com.ss.android.ugc.trill",
    "com.zhiliaoapp.musically.go",
    "com.grabtaxi.passenger",
};

constexpr size_t DEFAULT_TARGETS_COUNT = sizeof(DEFAULT_TARGETS) / sizeof(DEFAULT_TARGETS[0]);

// Partition subdirs used by generate_mount_files() to synthesize per-partition build.prop.
struct PartitionDir {
    const char* dir;   // subdirectory name under MOUNTDIR (e.g. "system")
    const char* pfx;   // ro.product.<partition>. prefix
};

constexpr PartitionDir PARTITIONS[] = {
    {"system",      "ro.product.system."},
    {"vendor",      "ro.product.vendor."},
    {"odm",         "ro.product.odm."},
    {"product",     "ro.product.product."},
    {"system_ext",  "ro.product.system_ext."},
};

constexpr size_t PARTITIONS_COUNT = sizeof(PARTITIONS) / sizeof(PARTITIONS[0]);

} // namespace ternak_tt
