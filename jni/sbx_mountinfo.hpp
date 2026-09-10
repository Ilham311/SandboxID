#pragma once

#include "config.hpp"

#include <string>
#include <vector>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace sbxmnt {

struct MountRow {
    std::string mount_point;
    std::string fstype;
    std::string source;
};

inline bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && std::memcmp(s.data(), p.data(), p.size()) == 0;
}

inline bool icontains(const std::string& hay, const char* needle) {
    std::string h = hay, ndl = needle;
    for (char& c : h)   c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : ndl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(ndl) != std::string::npos;
}

inline bool parse_mountinfo_line(const std::string& line, MountRow& out) {
    std::vector<std::string> tok;
    size_t i = 0, n = line.size();
    while (i < n) {
        while (i < n && line[i] == ' ') ++i;
        if (i >= n) break;
        size_t j = i;
        while (j < n && line[j] != ' ') ++j;
        tok.emplace_back(line.substr(i, j - i));
        i = j;
    }
    if (tok.size() < 7) return false;

    size_t dash = std::string::npos;
    for (size_t k = 6; k < tok.size(); ++k) if (tok[k] == "-") { dash = k; break; }
    if (dash == std::string::npos || dash + 2 >= tok.size()) return false;
    out.mount_point = tok[4];
    out.fstype      = tok[dash + 1];
    out.source      = tok[dash + 2];
    return true;
}

inline bool is_protected(const MountRow& r) {
    const std::string& mp = r.mount_point;
    if (mp == "/" || mp == "/data") return true;
    static const char* const roots[] = {
        "/system", "/vendor", "/product", "/system_ext", "/odm",
    };
    for (const char* R : roots) if (mp == R) return true;

    const std::string moddir(sandboxid::MODDIR);
    if (starts_with(mp, moddir))       return true;
    if (starts_with(r.source, moddir)) return true;

    for (const auto& be : sandboxid::BIND_ENTRIES)
        if (mp == be.dst) return true;
    return false;
}

inline bool is_trace(const MountRow& r) {
    const std::string& mp  = r.mount_point;
    const std::string& src = r.source;
    const std::string& fs  = r.fstype;

    if (mp == "/data/adb" || starts_with(mp, "/data/adb/")) return true;
    if (starts_with(mp, "/debug_ramdisk"))                  return true;

    if (fs == "overlay" || fs == "tmpfs") {
        if (icontains(src, "magisk") || icontains(src, "worker") ||
            icontains(src, "workdir") || icontains(src, "apatch") ||
            icontains(src, "/ksu") || src == "KSU")
            return true;
    }

    if (icontains(src, "magisk")) return true;
    return false;
}

inline std::vector<std::string> select_umount_targets(const std::string& mountinfo) {
    std::vector<std::string> out;
    size_t i = 0, n = mountinfo.size();
    while (i < n) {
        size_t eol = mountinfo.find('\n', i);
        size_t end = (eol == std::string::npos) ? n : eol;
        if (end > i) {
            MountRow r;
            if (parse_mountinfo_line(mountinfo.substr(i, end - i), r) &&
                !is_protected(r) && is_trace(r))
                out.push_back(r.mount_point);
        }
        if (eol == std::string::npos) break;
        i = eol + 1;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}
