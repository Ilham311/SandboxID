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

inline constexpr char PERSONAS_FILE[] = "/data/adb/modules/sandboxid/personas.tsv";

inline constexpr char PERSONA_OVERRIDE[] = "/data/adb/modules/sandboxid/persona.override";

inline constexpr char IDENTITY_BAK[]  = "/data/adb/modules/sandboxid/identity.prop.bak";
inline constexpr char MODE_FILE[]     = "/data/adb/modules/sandboxid/identity.mode";

inline constexpr char CARRIER_CONF[]  = "/data/adb/modules/sandboxid/carrier.conf";
inline constexpr char CARRIERS_FILE[] = "/data/adb/modules/sandboxid/carriers.tsv";
inline constexpr char RESETPROP[]     = "/data/adb/modules/sandboxid/bin/resetprop-rs";

inline constexpr char ENABLE_HIDE[]   = "/data/adb/modules/sandboxid/enable_hide";

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
};
inline constexpr size_t BIND_ENTRIES_N = sizeof(BIND_ENTRIES) / sizeof(BIND_ENTRIES[0]);

inline constexpr const char* MOUNT_PARTS[] = {"system", "vendor", "odm", "product", "system_ext"};
inline constexpr size_t MOUNT_PARTS_N = sizeof(MOUNT_PARTS) / sizeof(MOUNT_PARTS[0]);
inline constexpr char LEGACY_SETTINGS_OVERLAY[] =
    "/data/adb/modules/sandboxid/mount/settings_secure.xml";

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
