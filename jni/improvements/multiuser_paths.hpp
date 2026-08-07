// ternak_tt v2 — multiuser_paths.hpp
// Item #7: enumerate /data/system/users/*/ instead of hardcoding user 0.

#pragma once

#include <dirent.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace tt {

// Returns list of /data/system/users/<uid> paths for every user on device.
inline std::vector<std::string> enumerate_user_dirs() {
    std::vector<std::string> out;
    const char* base = "/data/system/users";
    DIR* d = ::opendir(base);
    if (!d) {
        out.emplace_back("/data/system/users/0");
        return out;
    }
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 ||
            (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
        bool numeric = true;
        for (const char* p = e->d_name; *p; ++p) {
            if (*p < '0' || *p > '9') { numeric = false; break; }
        }
        if (!numeric) continue;
        std::string full = std::string(base) + "/" + e->d_name;
        struct stat st;
        if (::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            out.push_back(std::move(full));
        }
    }
    ::closedir(d);
    if (out.empty()) out.emplace_back("/data/system/users/0");
    return out;
}

struct DynBindEntry {
    std::string src_rel;
    std::string dst;
};

inline std::vector<DynBindEntry> build_secure_xml_bind_entries() {
    std::vector<DynBindEntry> out;
    for (const auto& u : enumerate_user_dirs()) {
        out.push_back({"settings_secure.xml", u + "/settings_secure.xml"});
    }
    return out;
}

}  // namespace tt
