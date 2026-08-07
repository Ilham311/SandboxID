// ternak_tt v2.1.2 — radio_util.hpp
// Item #8: RADIO baseband string per SoC generation (not always g5300q).
// v2.1.2 changes:
//   - Added OnePlus device codes (OP5F0FL1, OP595DL1) and board-based fallback (kalama SoC).
//   - Added board-name overload for device detection when codename unknown.

#pragma once

#include <string>
#include <cstring>
#include <cstdio>

namespace tt {

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
        return "CP";
    }
    if (device[0] == 'a' && device[1] >= '3' && device[1] <= '7') {
        return "CP";
    }

    // Snapdragon 8 Gen 3 (Xiaomi 14, Xiaomi 14 Pro)
    if (!std::strcmp(device, "shennong")) return "MPSS.HI.5.1";
    if (!std::strcmp(device, "aurora"))   return "MPSS.HI.5.1";

    // --- v2.1.2: Snapdragon 8 Gen 2 (kalama board) ---
    // Xiaomi 13 (fuxi), Xiaomi 13 Pro (nuwa), OnePlus 11 (salami), OnePlus Open (dumpling)
    if (!std::strcmp(device, "kalama"))   return "MPSS.HI.5.0";
    if (!std::strcmp(device, "fuxi"))     return "MPSS.HI.5.0";
    if (!std::strcmp(device, "nuwa"))     return "MPSS.HI.5.0";
    if (!std::strcmp(device, "salami"))   return "MPSS.HI.5.0";
    if (!std::strcmp(device, "dumpling")) return "MPSS.HI.5.0";

    // --- v2.1.2: OnePlus internal device codes ---
    // OP5F0FL1 = OnePlus Open (dumpling board, SD8G2)
    // OP595DL1 = OnePlus 12 (pineapple board, SD8G3)
    if (!std::strcmp(device, "OP5F0FL1")) return "MPSS.HI.5.0";
    if (!std::strcmp(device, "OP595DL1")) return "MPSS.HI.5.2";
    if (!std::strcmp(device, "pineapple")) return "MPSS.HI.5.2";

    // Fallback: legacy g5300q (v1 behavior)
    return "g5300q";
}

// Detect if device is Snapdragon-family (needs MPSS format).
inline bool is_snapdragon_device(const char* device) {
    if (!device) return false;
    const char* tag = baseband_tag_for_device(device);
    return std::strncmp(tag, "MPSS", 4) == 0;
}

// Detect if device is Samsung.
inline bool is_samsung_device(const char* device) {
    if (!device) return false;
    if (device[0] == 'e' && device[1] >= '1' && device[1] <= '3') return true;
    if (device[0] == 'a' && device[1] >= '3' && device[1] <= '7') return true;
    return false;
}

inline std::string format_radio(const char* device, const char* incremental,
                                const char* date_yymmdd) {
    const char* tag = baseband_tag_for_device(device);
    char buf[192];

    if (is_samsung_device(device)) {
        std::snprintf(buf, sizeof(buf), "%s", incremental ? incremental : "");
        return buf;
    }

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
