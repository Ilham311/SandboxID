

#pragma once

#include <string>
#include <cstring>
#include <cstdio>

namespace tt {

inline const char* baseband_tag_for_device(const char* device) {
    if (!device) return "g5300q";

    if (!std::strcmp(device, "oriole"))  return "g5123b";
    if (!std::strcmp(device, "raven"))   return "g5123b";
    if (!std::strcmp(device, "bluejay")) return "g5123b";

    if (!std::strcmp(device, "panther")) return "g5300a";
    if (!std::strcmp(device, "cheetah")) return "g5300a";
    if (!std::strcmp(device, "lynx"))    return "g5300a";

    if (!std::strcmp(device, "shiba"))   return "g5300q";
    if (!std::strcmp(device, "husky"))   return "g5300q";
    if (!std::strcmp(device, "akita"))   return "g5300q";

    if (!std::strcmp(device, "tokay"))   return "g5400t";
    if (!std::strcmp(device, "caiman"))  return "g5400t";
    if (!std::strcmp(device, "komodo"))  return "g5400t";
    if (!std::strcmp(device, "tegu"))    return "g5400t";

    if (!std::strcmp(device, "frankel")) return "g5500u";
    if (!std::strcmp(device, "blazer"))  return "g5500u";

    if (device[0] == 'e' && (device[1] >= '1' && device[1] <= '3')) {
        return "CP";
    }
    if (device[0] == 'a' && device[1] >= '3' && device[1] <= '7') {
        return "CP";
    }

    if (!std::strcmp(device, "shennong")) return "MPSS.HI.5.1";
    if (!std::strcmp(device, "aurora"))   return "MPSS.HI.5.1";
    if (!std::strcmp(device, "kalama"))   return "MPSS.HI.5.0";

    return "g5300q";
}

inline std::string format_radio(const char* device, const char* incremental,
                                const char* date_yymmdd) {
    const char* tag = baseband_tag_for_device(device);
    char buf[192];

    if (device && device[0] == 'e' &&
        device[1] >= '1' && device[1] <= '3') {
        std::snprintf(buf, sizeof(buf), "%s", incremental ? incremental : "");
        return buf;
    }
    if (device && device[0] == 'a' && device[1] >= '3' && device[1] <= '7') {
        std::snprintf(buf, sizeof(buf), "%s", incremental ? incremental : "");
        return buf;
    }

    if (std::strncmp(tag, "MPSS", 4) == 0) {
        std::snprintf(buf, sizeof(buf), "%s-%s",
                      tag, incremental ? incremental : "unknown");
        return buf;
    }

    std::snprintf(buf, sizeof(buf), "%s-%s-%s-B-%s",
                  tag,
                  date_yymmdd ? date_yymmdd : "260101",
                  date_yymmdd ? date_yymmdd : "260101",
                  incremental ? incremental : "unknown");
    return buf;
}

}
