#pragma once
//
// tt_config.hpp — single source of truth shared by main.cpp (Zygisk module),
// companion.cpp (root daemon) and, where relevant, ternak-tt.cpp (CLI).
//
// Rationale (why this file exists): paths, the IPC command set, the bind-mount
// table and the fallback property values used to be duplicated across main.cpp
// and companion.cpp with subtly *different* contents (e.g. main.cpp listed 6
// bind entries, companion 9; both hard-coded a Qualcomm SoC that contradicts the
// Pixel persona). Centralising them here makes the data auditable in one place
// and removes the drift.
//
// Header-only, C++17 inline variables. Safe under -fno-exceptions/-fno-rtti.

#include <cstdint>
#include <cstddef>
#include <unistd.h>
#include <cerrno>

namespace tt {

// ------------------------------------------------------------------ paths ---
inline constexpr char MODDIR[]        = "/data/adb/modules/ternak_tt";
inline constexpr char IDENTITY_FILE[] = "/data/adb/modules/ternak_tt/identity.prop";
inline constexpr char MOUNTDIR[]      = "/data/adb/modules/ternak_tt/mount";
inline constexpr char TARGET_FILE[]   = "/data/adb/modules/ternak_tt/target.txt";

// CLI-only paths (ternak-tt.cpp). Centralised here so every path literal lives
// in one place and cannot drift between the daemon and the CLI.
inline constexpr char IDENTITY_BAK[]  = "/data/adb/modules/ternak_tt/identity.prop.bak";
inline constexpr char MODE_FILE[]     = "/data/adb/modules/ternak_tt/identity.mode";
inline constexpr char RESETPROP[]     = "/data/adb/modules/ternak_tt/bin/resetprop-rs";

// --------------------------------------------------------------- IPC proto ---
// One byte command, then command-specific framing. Client and companion are
// always the same build (compiled from this repo, shipped together), so the
// protocol needs no cross-version negotiation — but every field is still
// length-checked via read_full/write_full so a truncated/rogue peer can never
// desync us into reading attacker-controlled lengths unbounded.
enum Cmd : uint8_t {
    CMD_CHECK_TT     = 1,
    CMD_GET_IDENTITY = 2,   // [u16 plen][pkg] -> [u32 len][identity.prop bytes]
    CMD_DO_MOUNTS    = 3,   // [u32 pid]       -> [u32 mounts_ok]
};

// Upper bound on an identity blob we will accept from the companion. identity.prop
// is < 2 KiB in practice; this guards resize() against a corrupt length.
inline constexpr uint32_t MAX_IDENTITY_BLOB = 64u * 1024u;

// ------------------------------------------------------- bind-mount table ---
struct BindEntry { const char* src_rel; const char* dst; };

// The real, complete list the companion mounts. Both the "/x/etc/build.prop"
// and "/x/build.prop" layouts are attempted; non-existent destinations are
// skipped (access(F_OK)) so this is a superset that adapts per device.
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

// Partition subdirs under MOUNTDIR that hold a synthetic build.prop. The CLI's
// generate_mount_files() writes these; BIND_ENTRIES maps them to real targets.
inline constexpr const char* MOUNT_PARTS[] = {"system", "vendor", "odm", "product", "system_ext"};
inline constexpr size_t MOUNT_PARTS_N = sizeof(MOUNT_PARTS) / sizeof(MOUNT_PARTS[0]);

// ---------------------------------------------------- property fallbacks ---
// Auditable default tables (moved out of val()/hook_prop_get so a reviewer can
// see every value the module might feed an app without reading hook logic).
struct KV { const char* k; const char* v; };

// Fallbacks for val(): used only when g_id has no non-empty entry for the key.
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

// Static property answers that are identity-independent AND safe/consistent for
// a Pixel persona. NOTE: ro.hardware and ro.board.platform were REMOVED from
// this table — they used to claim a Qualcomm SoC ("qcom"/"sm8250") which
// contradicts a Google/Tensor Pixel and is trivially detectable. Those two keys
// are now driven from the identity (HARDWARE / BOARD_PLATFORM) in hook_prop_get.
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

// --------------------------------------------------------- socket helpers ---
// Loop until exactly n bytes are transferred or the peer/erroring stops us.
// EINTR is retried; short reads/writes are continued. Returns false on EOF or
// hard error so callers can bail instead of proceeding on partial data.
inline bool read_full(int fd, void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, p + got, n - got);
        if (r > 0) { got += static_cast<size_t>(r); continue; }
        if (r < 0 && errno == EINTR) continue;
        return false;   // 0 = EOF, <0 = error
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

} // namespace tt
