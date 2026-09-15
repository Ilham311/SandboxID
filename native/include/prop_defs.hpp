#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

namespace sandboxid {

struct NativePropEntry {
    const char* key;
    const char* val;
    const char* identity_val;
    bool del_if_empty;
};

inline constexpr NativePropEntry NATIVE_PROPS[] = {
    {"ro.serialno",                        nullptr, "SERIAL", false},
    {"ro.boot.serialno",                   nullptr, "SERIAL", false},
    {"ro.build.fingerprint",               nullptr, "FINGERPRINT", false},
    {"ro.bootimage.build.fingerprint",     nullptr, "FINGERPRINT", false},
    {"ro.system.build.fingerprint",        nullptr, "FINGERPRINT", false},
    {"ro.vendor.build.fingerprint",        nullptr, "FINGERPRINT", false},
    {"ro.odm.build.fingerprint",           nullptr, "FINGERPRINT", false},
    {"ro.product.build.fingerprint",       nullptr, "FINGERPRINT", false},
    {"ro.system_ext.build.fingerprint",    nullptr, "FINGERPRINT", false},
    {"ro.vendor_dlkm.build.fingerprint",   nullptr, "FINGERPRINT", false},
    {"ro.odm_dlkm.build.fingerprint",      nullptr, "FINGERPRINT", false},
    {"ro.product.model",                   nullptr, "MODEL", false},
    {"ro.product.system.model",            nullptr, "MODEL", false},
    {"ro.product.vendor.model",            nullptr, "MODEL", false},
    {"ro.product.odm.model",               nullptr, "MODEL", false},
    {"ro.product.product.model",           nullptr, "MODEL", false},
    {"ro.product.system_ext.model",        nullptr, "MODEL", false},
    {"ro.product.brand",                   nullptr, "BRAND", false},
    {"ro.product.system.brand",            nullptr, "BRAND", false},
    {"ro.product.vendor.brand",            nullptr, "BRAND", false},
    {"ro.product.odm.brand",               nullptr, "BRAND", false},
    {"ro.product.product.brand",           nullptr, "BRAND", false},
    {"ro.product.system_ext.brand",        nullptr, "BRAND", false},
    {"ro.product.manufacturer",            nullptr, "MANUFACTURER", false},
    {"ro.product.system.manufacturer",     nullptr, "MANUFACTURER", false},
    {"ro.product.vendor.manufacturer",     nullptr, "MANUFACTURER", false},
    {"ro.product.odm.manufacturer",        nullptr, "MANUFACTURER", false},
    {"ro.product.product.manufacturer",    nullptr, "MANUFACTURER", false},
    {"ro.product.system_ext.manufacturer", nullptr, "MANUFACTURER", false},
    {"ro.product.device",                  nullptr, "DEVICE", false},
    {"ro.product.system.device",           nullptr, "DEVICE", false},
    {"ro.product.vendor.device",           nullptr, "DEVICE", false},
    {"ro.product.odm.device",              nullptr, "DEVICE", false},
    {"ro.product.product.device",          nullptr, "DEVICE", false},
    {"ro.product.system_ext.device",       nullptr, "DEVICE", false},
    {"ro.product.name",                    nullptr, "PRODUCT", false},
    {"ro.product.system.name",             nullptr, "PRODUCT", false},
    {"ro.product.vendor.name",             nullptr, "PRODUCT", false},
    {"ro.product.odm.name",                nullptr, "PRODUCT", false},
    {"ro.product.product.name",            nullptr, "PRODUCT", false},
    {"ro.product.system_ext.name",         nullptr, "PRODUCT", false},
    {"ro.build.product",                   nullptr, "DEVICE", false},
    {"ro.soc.manufacturer",                nullptr, "SOC_MANUFACTURER", false},
    {"ro.soc.model",                       nullptr, "SOC_MODEL", false},
    {"ro.product.marketname",              nullptr, "MARKETNAME", false},
    {"ro.product.brand_for_attestation",        nullptr, "BRAND", false},
    {"ro.product.name_for_attestation",         nullptr, "PRODUCT", false},
    {"ro.product.device_for_attestation",       nullptr, "DEVICE", false},
    {"ro.product.model_for_attestation",        nullptr, "MODEL", false},
    {"ro.product.manufacturer_for_attestation", nullptr, "MANUFACTURER", false},
    {"ro.build.id",                        nullptr, "ID", false},
    {"ro.build.display.id",                nullptr, "DISPLAY", false},
    {"ro.build.description",               nullptr, "DESCRIPTION", false},
    {"ro.build.tags",                      nullptr, "TAGS", false},
    {"ro.build.type",                      nullptr, "TYPE", false},
    {"ro.build.user",                      nullptr, "USER", false},
    {"ro.build.host",                      nullptr, "HOST", false},
    {"ro.build.flavor",                    nullptr, "FLAVOR", false},
    {"ro.build.date.utc",                  nullptr, "BUILD_TIME_UTC", false},
    {"ro.build.date",                      nullptr, "BUILD_DATE", false},
    {"ro.build.version.codename",          "REL", "", false},
    {"ro.build.version.all_codenames",     "REL", "", false},
    {"ro.build.version.release",           nullptr, "RELEASE", false},
    {"ro.build.version.release_or_codename", nullptr, "RELEASE", false},
    {"ro.build.version.security_patch",    nullptr, "SECURITY_PATCH", false},
    {"ro.vendor.build.security_patch",     nullptr, "SECURITY_PATCH", false},
    {"ro.build.version.incremental",       nullptr, "INCREMENTAL", false},
    {"gsm.version.baseband",               nullptr, "RADIO", true},
    {"ro.build.expect.baseband",           nullptr, "RADIO", true},
    {"ro.bootloader",                      "unknown", "", false},
    {"ro.boot.bootloader",                 "unknown", "", false},
    {"ro.boot.verifiedbootstate",          "green", "", false},
    {"ro.boot.vbmeta.device_state",        "locked", "", false},
    {"ro.boot.flash.locked",               "1", "", false},
    {"ro.boot.veritymode",                 "enforcing", "", false},
    {"ro.boot.vbmeta.hash_alg",            "sha256", "", false},
    {"ro.boot.vbmeta.avb_version",         "1.0", "", false},
    {"ro.boot.vbmeta.invalidate_on_error", "yes", "", false},
    {"ro.boot.vbmeta.digest",              nullptr, "VBMETA_DIGEST", false},
    {"ro.secure",                          "1", "", false},
    {"ro.debuggable",                      "0", "", false},
    {"ro.build.selinux",                   "1", "", false},
    {"sys.oem_unlock_allowed",             "0", "", false},
    {"ro.boot.hardware.sku",               nullptr, "SKU", false},
    {"ro.boot.product.hardware.sku",       nullptr, "ODM_SKU", false},
    {"ro.build.version.base_os",           nullptr, "BASE_OS", false},
    {"ro.build.version.preview_sdk",       "0", "", false},
    {"ro.build.version.preview_sdk_fingerprint", "REL", "", false},
    {"ro.odm.build.media_performance_class", nullptr, "MEDIA_PERFORMANCE_CLASS", false},
    // ---- Missing Build-level aliases (from DeviceSpoofLab research) ----
    {"ro.build.brand",                      nullptr, "BRAND", false},
    {"ro.build.manufacturer",               nullptr, "MANUFACTURER", false},
    {"ro.build.device",                     nullptr, "DEVICE", false},
    {"ro.product.vendor_dlkm.brand",        nullptr, "BRAND", false},
    {"ro.product.vendor_dlkm.manufacturer", nullptr, "MANUFACTURER", false},
    {"ro.product.vendor_dlkm.model",        nullptr, "MODEL", false},
    {"ro.product.vendor_dlkm.name",         nullptr, "PRODUCT", false},
    {"ro.product.vendor_dlkm.device",       nullptr, "DEVICE", false},
    {"ro.product.bootimage.brand",          nullptr, "BRAND", false},
    {"ro.product.bootimage.manufacturer",   nullptr, "MANUFACTURER", false},
    {"ro.product.bootimage.model",          nullptr, "MODEL", false},
    {"ro.product.bootimage.name",           nullptr, "PRODUCT", false},
    {"ro.product.bootimage.device",         nullptr, "DEVICE", false},
    {"ro.product.system_dlkm.brand",        nullptr, "BRAND", false},
    {"ro.product.system_dlkm.manufacturer", nullptr, "MANUFACTURER", false},
    {"ro.product.system_dlkm.model",        nullptr, "MODEL", false},
    {"ro.product.system_dlkm.name",         nullptr, "PRODUCT", false},
    {"ro.product.system_dlkm.device",       nullptr, "DEVICE", false},
    {"ro.system_dlkm.build.fingerprint",    nullptr, "FINGERPRINT", false},
    {"ro.product.vendor_dlkm.build.fingerprint", nullptr, "FINGERPRINT", false},
    {"ro.product.bootimage.build.fingerprint",   nullptr, "FINGERPRINT", false},
    // ---- Partition build versions ----
    {"ro.vendor.build.version.release",            nullptr, "RELEASE", false},
    {"ro.vendor.build.version.release_or_codename", nullptr, "RELEASE", false},
    {"ro.vendor_dlkm.build.version.release",       nullptr, "RELEASE", false},
    {"ro.vendor_dlkm.build.version.release_or_codename", nullptr, "RELEASE", false},
    {"ro.odm.build.version.release",               nullptr, "RELEASE", false},
    {"ro.odm.build.version.release_or_codename",   nullptr, "RELEASE", false},
    {"ro.bootimage.build.version.release",         nullptr, "RELEASE", false},
    {"ro.bootimage.build.version.release_or_codename", nullptr, "RELEASE", false},
    {"ro.system_dlkm.build.version.release",       nullptr, "RELEASE", false},
    {"ro.system_dlkm.build.version.release_or_codename", nullptr, "RELEASE", false},
    // ---- Product build metadata ----
    {"ro.product.build.id",     nullptr, "ID", false},
    {"ro.product.build.tags",   nullptr, "TAGS", false},
    {"ro.product.build.type",   nullptr, "TYPE", false},
    {"ro.product.build.version.incremental",  nullptr, "INCREMENTAL", false},
    {"ro.product.build.version.release",      nullptr, "RELEASE", false},
    {"ro.product.build.version.release_or_codename", nullptr, "RELEASE", false},
    {"ro.product.build.version.sdk",          nullptr, "SDK_INT", false},
    // ---- Anti-emulator / security ----
    {"ro.kernel.qemu",          "0", "", false},
    {"ro.boot.qemu",            "0", "", false},
    {"ro.adb.secure",           "1", "", false},
    {"ro.boot.warranty_bit",    "0", "", false},
    {"ro.crypto.state",         "encrypted", "", false},
    {"ro.treble.enabled",       "true", "", false},
    {"persist.sys.usb.config",  "none", "", false},
    // ---- Hardware / arch / other ----
    {"ro.boot.hardware",        nullptr, "HARDWARE", false},
    {"ro.boot.mode",            "normal", "", false},
    {"ro.arch",                 "arm64", "", false},
    {"gsm.sim.operator.iso-country", nullptr, "GSM_OPERATOR_ISO", false},
    {"ro.sf.lcd_density",       nullptr, "LCD_DENSITY", false},
};

inline constexpr size_t NATIVE_PROPS_N = sizeof(NATIVE_PROPS) / sizeof(NATIVE_PROPS[0]);

struct MountPartEntry {
    const char* dir;
    const char* prefix;
};

inline constexpr MountPartEntry MOUNT_PART_ENTRIES[] = {
    {"system",     "ro.product.system."},
    {"vendor",     "ro.product.vendor."},
    {"odm",        "ro.product.odm."},
    {"product",    "ro.product.product."},
    {"system_ext", "ro.product.system_ext."},
};

inline constexpr size_t MOUNT_PART_ENTRIES_N = sizeof(MOUNT_PART_ENTRIES) / sizeof(MOUNT_PART_ENTRIES[0]);

inline std::string generate_mount_part_props(const std::string& prefix,
                                              const std::string& model,
                                              const std::string& brand,
                                              const std::string& manufacturer,
                                              const std::string& device,
                                              const std::string& product) {
    std::string out;
    if (!model.empty())        out += prefix + "model="        + model        + "\n";
    if (!brand.empty())        out += prefix + "brand="        + brand        + "\n";
    if (!manufacturer.empty()) out += prefix + "manufacturer=" + manufacturer + "\n";
    if (!device.empty())       out += prefix + "device="       + device       + "\n";
    if (!product.empty())      out += prefix + "name="         + product      + "\n";
    return out;
}

}