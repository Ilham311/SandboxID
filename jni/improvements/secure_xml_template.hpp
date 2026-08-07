

#pragma once

#include <string>

namespace tt {

inline void xml_escape_append(std::string& out, const std::string& v) {
    for (char c : v) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;        break;
        }
    }
}

inline void append_setting(std::string& out, int id, const char* name,
                           const std::string& value, const char* pkg) {
    out += "  <setting id=\"";
    out += std::to_string(id);
    out += "\" name=\"";
    xml_escape_append(out, name);
    out += "\" value=\"";
    xml_escape_append(out, value);
    out += "\" package=\"";
    xml_escape_append(out, pkg);
    out += "\" />\n";
}

inline std::string build_secure_xml(const std::string& android_id,
                                    const std::string& gaid,
                                    int settings_version = 210) {
    std::string xml;
    xml.reserve(2048);
    xml += "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n";
    xml += "<settings version=\"";
    xml += std::to_string(settings_version);
    xml += "\">\n";

    int id = 1;
    append_setting(xml, id++, "android_id",
                   android_id.empty() ? "0000000000000000" : android_id,
                   "android");
    append_setting(xml, id++, "install_non_market_apps", "1", "android");
    append_setting(xml, id++, "user_setup_complete",     "1", "android");

    if (!gaid.empty()) {
        append_setting(xml, id++, "advertising_id",     gaid, "com.google.android.gms");
        append_setting(xml, id++, "limit_ad_tracking",  "0",  "com.google.android.gms");
    }

    append_setting(xml, id++, "development_settings_enabled", "0", "android");
    append_setting(xml, id++, "adb_enabled",                  "0", "android");
    append_setting(xml, id++, "install_non_market_apps",      "1", "android");

    xml += "</settings>\n";
    return xml;
}

}
