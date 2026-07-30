#pragma once
#include <cstring>

static inline bool is_sensitive_proc_path(const char* p) {
    if (!p) return false;
    if (!strstr(p, "/proc/")) return false;
    if (strstr(p, "/mountinfo")) return true;
    if (strstr(p, "/mounts"))    return true;
    if (strstr(p, "/maps"))      return true;
    return false;
}
