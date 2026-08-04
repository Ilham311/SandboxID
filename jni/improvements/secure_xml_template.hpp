// ternak_tt v2 — secure_xml_template.hpp
// Item #2 (CRITICAL): proper Android settings_secure.xml template.
//
// USAGE (in ternak-tt.cpp::generate_mount_files, replace the existing
// XML-building block with):
//   #include "improvements/secure_xml_template.hpp"
//   std::string xml = tt::build_secure_xml(aid, gaid, /*version=*/210);
//   std::string xml_path = std::string(MOUNTDIR) + "/settings_secure.xml";
//   atomic_write(xml_path, xml);
//   ::chmod(xml_path.c_str(), 0600);
//   ::chown(xml_path.c_str(), 1000, 1000);
//
// The file matches the format Android's SettingsProvider actually persists,
// so a bind-mount over /data/system/users/<uid>/settings_secure.xml is a
// drop-in replacement (not blank-out).
//
// Reference: frameworks/base/packages/SettingsProvider/src/com/android/providers/settings/SettingsProvider.java
// Real file example:
//   <?xml version='1.0' encoding='utf-8' standalone='yes' ?>
//   <settings version="210">
//     <setting id="1" name="android_id" value="a1b2c3…" package="android" />
//     ...
//   </settings>

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

// Build a valid Android settings_secure.xml with the given android_id and
// optional advertising_id (GAID). Additional entries are safe defaults that
// keep target apps happy without exposing device identity.
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

    // Defense-in-depth: neutralize a few keys that some anti-fraud SDKs
    // read directly (safe defaults that mimic a fresh device).
    append_setting(xml, id++, "development_settings_enabled", "0", "android");
    append_setting(xml, id++, "adb_enabled",                  "0", "android");
    append_setting(xml, id++, "install_non_market_apps",      "1", "android");

    xml += "</settings>\n";
    return xml;
}

}  // namespace tt
