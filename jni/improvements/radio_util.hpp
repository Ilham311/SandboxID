// ternak_tt v2 — radio_util.hpp
// Item #8: RADIO baseband string per SoC generation (not always g5300q).
//
// USAGE (in ternak-tt.cpp::gen_identity):
//   #include "improvements/radio_util.hpp"
//   char rad[128];
//   std::string radio = tt::format_radio(p.device, p.incremental, date_yymmdd);
//   id.kv["RADIO"] = radio;

#pragma once

#include <string>
#include <cstring>
#include <cstdio>

namespace tt {

// Detect Tensor / Snapdragon generation from Pixel device codename.
// Returns baseband tag prefix used in real Pixel factory images.
inline const char* baseband_tag_for_device(const char* device) {
    if (!device) return "g5300q";

    // Tensor G1 (Pixel 6, 6 Pro, 6a) — Exynos 5123b modem
    if (!std::strcmp(device, "oriole"))  return "g5123b";
    if (!std::strcmp(device, "raven"))   return "g5123b";
    if (!std::strcmp(device, "bluejay")) return "g5123b";

    // Tensor G2 (Pixel 7, 7 Pro, 7a)
    if (!std::strcmp(device, "panther")) return "g5300a";
    if (!std::strcmp(device, "cheetah")) return "g5300a";
    if (!std::strcmp(device, "lynx"))    return "g5300a";

    // Tensor G3 (Pixel 8, 8 Pro, 8a)
    if (!std::strcmp(device, "shiba"))   return "g5300q";
    if (!std::strcmp(device, "husky"))   return "g5300q";
    if (!std::strcmp(device, "akita"))   return "g5300q";

    // Tensor G4 (Pixel 9 series)
    if (!std::strcmp(device, "tokay"))   return "g5400t";
    if (!std::strcmp(device, "caiman"))  return "g5400t";
    if (!std::strcmp(device, "komodo"))  return "g5400t";
    if (!std::strcmp(device, "tegu"))    return "g5400t";

    // Tensor G5 (Pixel 10 series)
    if (!std::strcmp(device, "frankel")) return "g5500u";
    if (!std::strcmp(device, "blazer"))  return "g5500u";

    // Samsung Galaxy — model-derived
    if (device[0] == 'e' && (device[1] >= '1' && device[1] <= '3')) {
        return "CP";   // e1q/e2q/e3q → CP<model>XXU
    }
    if (device[0] == 'a' && device[1] >= '3' && device[1] <= '7') {
        return "CP";
    }

    // Snapdragon (Xiaomi, OnePlus, Realme)
    if (!std::strcmp(device, "shennong")) return "MPSS.HI.5.1";
    if (!std::strcmp(device, "aurora"))   return "MPSS.HI.5.1";
    if (!std::strcmp(device, "kalama"))   return "MPSS.HI.5.0";

    // Fallback: legacy g5300q (v1 behavior)
    return "g5300q";
}

// Full RADIO string in the format Android expects.
//   Pixel:    <tag>-<yymmdd>-<yymmdd>-B-<incremental>
//   Samsung:  <tag><model>XXU<incremental>
//   Snapdragon: <tag>-<incremental>
inline std::string format_radio(const char* device, const char* incremental,
                                const char* date_yymmdd) {
    const char* tag = baseband_tag_for_device(device);
    char buf[192];

    // Samsung path: incremental IS the radio string basically
    if (device && device[0] == 'e' &&
        device[1] >= '1' && device[1] <= '3') {
        std::snprintf(buf, sizeof(buf), "%s", incremental ? incremental : "");
        return buf;
    }
    if (device && device[0] == 'a' && device[1] >= '3' && device[1] <= '7') {
        std::snprintf(buf, sizeof(buf), "%s", incremental ? incremental : "");
        return buf;
    }

    // Snapdragon (MPSS)
    if (std::strncmp(tag, "MPSS", 4) == 0) {
        std::snprintf(buf, sizeof(buf), "%s-%s",
                      tag, incremental ? incremental : "unknown");
        return buf;
    }

    // Pixel/Tensor default
    std::snprintf(buf, sizeof(buf), "%s-%s-%s-B-%s",
                  tag,
                  date_yymmdd ? date_yymmdd : "260101",
                  date_yymmdd ? date_yymmdd : "260101",
                  incremental ? incremental : "unknown");
    return buf;
}

}  // namespace tt
