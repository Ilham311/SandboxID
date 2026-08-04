// secure_xml_gaid.hpp
//
// Drop-in REPLACEMENT for improvements/secure_xml_template.hpp from ternak_tt_v2.zip.
// Adds the Google Play Services ad-ID row(s) that were missing — this is the fix
// for the "GAID: I/O error" red flag.
//
// Kenapa string XML besar ini penting:
//   AdvertisingIdClient.getAdvertisingIdInfo() men-bind ke Play Services yang
//   membaca Settings.Secure lewat ContentProvider. Kalau row `advertising_id`
//   TIDAK ADA, provider melempar RemoteException; SDK translate ke IOException;
//   app melihat "I/O error". Kalau row ADA + valid UUID + limit_ad_tracking=0,
//   Play Services return normal UUID → anti-fraud lihat device wajar.
//
// Bind-mount path (per user): /data/system/users/<uid>/settings_secure.xml.
// Kita generate satu string XML lengkap sekali per boot, di-cache di
// MOUNTDIR/settings_secure.xml, dan di-mount ke lokasi target.

#pragma once

#include <string>
#include <sstream>
#include "random_util.hpp"   // dari v2 pack

namespace ttfix {

// GAID = UUID v4, lower-case, hyphenated. Anti-fraud punya regex.
inline std::string gen_gaid_uuid() {
    // 16 byte random, set version(4) + variant(10xx) per RFC 4122.
    unsigned char b[16];
    ttfix::urandom_bytes(b, 16);
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    static const char* hx = "0123456789abcdef";
    char out[37];
    int p = 0;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[p++] = '-';
        out[p++] = hx[(b[i] >> 4) & 0xF];
        out[p++] = hx[b[i] & 0xF];
    }
    out[p] = '\0';
    return std::string(out);
}

// One row of _settings table serialized as XML.
inline void emit_row(std::ostringstream& os, int id, const char* name,
                     const std::string& value, const char* package = "android") {
    os << "  <setting id=\"" << id << "\" name=\"" << name
       << "\" value=\"" << value
       << "\" package=\"" << package << "\" />\n";
}

// Build the full settings_secure.xml body. `gaid` should be a stable UUID for
// this identity generation (see gaid_persistence.hpp).
inline std::string build_secure_xml(const std::string& android_id_hex,
                                    const std::string& gaid) {
    std::ostringstream os;
    os << "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n";
    os << "<settings version=\"217\">\n";

    int id = 1;

    // --- Google Play Services ad ID surface (THE fix) ---
    // Play Services baca 3 key ini. Semua tiga harus ada dan sinkron.
    emit_row(os, id++, "advertising_id",         gaid,               "com.google.android.gms");
    emit_row(os, id++, "limit_ad_tracking",      "0",                "com.google.android.gms");
    // Beberapa build cek juga `ad_id_settings_toggle` (Android 13+).
    emit_row(os, id++, "ad_id_settings_toggle",  "1",                "com.google.android.gms");

    // --- Developer mode / debugging surface ---
    emit_row(os, id++, "development_settings_enabled", "0");
    emit_row(os, id++, "adb_enabled",                  "0");
    emit_row(os, id++, "adb_wifi_enabled",             "0");
    emit_row(os, id++, "install_non_market_apps",      "0");

    // --- Android ID (must match L1 build fingerprint ANDROID_ID) ---
    emit_row(os, id++, "android_id", android_id_hex);

    // --- Common location / accessibility spoof surface ---
    emit_row(os, id++, "location_providers_allowed",      "gps,network");
    emit_row(os, id++, "location_mode",                   "3");
    emit_row(os, id++, "mock_location",                   "0");
    emit_row(os, id++, "accessibility_enabled",           "0");
    emit_row(os, id++, "enabled_accessibility_services",  "");
    emit_row(os, id++, "touch_exploration_enabled",       "0");
    emit_row(os, id++, "screensaver_enabled",             "1");
    emit_row(os, id++, "user_setup_complete",             "1");

    // --- Bluetooth address (some SDK read Settings.Secure("bluetooth_address")) ---
    // Sinkron dengan bluetooth prop kalau ada, kalau tidak biarkan kosong.
    emit_row(os, id++, "bluetooth_name",                  "Pixel 10");

    os << "</settings>\n";
    return os.str();
}

} // namespace ttfix
