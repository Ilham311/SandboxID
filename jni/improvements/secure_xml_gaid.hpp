

#pragma once

#include <string>
#include <sstream>
#include "random_util.hpp"

namespace ttfix {

inline std::string gen_gaid_uuid() {

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

inline void emit_row(std::ostringstream& os, int id, const char* name,
                     const std::string& value, const char* package = "android") {
    os << "  <setting id=\"" << id << "\" name=\"" << name
       << "\" value=\"" << value
       << "\" package=\"" << package << "\" />\n";
}

inline std::string build_secure_xml(const std::string& android_id_hex,
                                    const std::string& gaid) {
    std::ostringstream os;
    os << "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n";
    os << "<settings version=\"217\">\n";

    int id = 1;

    emit_row(os, id++, "advertising_id",         gaid,               "com.google.android.gms");
    emit_row(os, id++, "limit_ad_tracking",      "0",                "com.google.android.gms");

    emit_row(os, id++, "ad_id_settings_toggle",  "1",                "com.google.android.gms");

    emit_row(os, id++, "development_settings_enabled", "0");
    emit_row(os, id++, "adb_enabled",                  "0");
    emit_row(os, id++, "adb_wifi_enabled",             "0");
    emit_row(os, id++, "install_non_market_apps",      "0");

    emit_row(os, id++, "android_id", android_id_hex);

    emit_row(os, id++, "location_providers_allowed",      "gps,network");
    emit_row(os, id++, "location_mode",                   "3");
    emit_row(os, id++, "mock_location",                   "0");
    emit_row(os, id++, "accessibility_enabled",           "0");
    emit_row(os, id++, "enabled_accessibility_services",  "");
    emit_row(os, id++, "touch_exploration_enabled",       "0");
    emit_row(os, id++, "screensaver_enabled",             "1");
    emit_row(os, id++, "user_setup_complete",             "1");

    emit_row(os, id++, "bluetooth_name",                  "Pixel 10");

    os << "</settings>\n";
    return os.str();
}

}
