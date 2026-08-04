// ternak_tt v2 — multiuser_paths.hpp
// Item #7: enumerate /data/system/users/*/ instead of hardcoding user 0.
//
// USAGE (in companion.cpp, replace the static BIND_ENTRIES for
// settings_secure.xml with dynamic enumeration):
//
//   #include "improvements/multiuser_paths.hpp"
//   ...
//   // Static build.prop entries as before
//   static const BindEntry BIND_ENTRIES_STATIC[] = {
//       {"system/build.prop",     "/system/build.prop"},
//       {"vendor/build.prop",     "/vendor/build.prop"},
//       ...
//   };
//
//   // Then, when building the per-request BindEntry list:
//   std::vector<BindEntry> entries(std::begin(BIND_ENTRIES_STATIC),
//                                  std::end(BIND_ENTRIES_STATIC));
//   for (const auto& u : tt::enumerate_user_dirs()) {
//       BindEntry be;
//       be.src_rel = "settings_secure.xml";
//       // NOTE: dst string is owned by the caller — keep it alive until mount()
//       static thread_local std::string keep;
//       keep = u + "/settings_secure.xml";
//       be.dst = keep.c_str();
//       entries.push_back(be);
//   }
//
// Or the RAII-friendly wrapper `build_bind_entries()` at bottom of this file.

#pragma once

#include <dirent.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace tt {

// Returns list of /data/system/users/<uid> paths for every user on device.
// On a single-user device this is just ["/data/system/users/0"].
// On multi-user or Work Profile devices this includes 10, 11, ...
inline std::vector<std::string> enumerate_user_dirs() {
    std::vector<std::string> out;
    const char* base = "/data/system/users";
    DIR* d = ::opendir(base);
    if (!d) {
        // Fallback to user 0 only if enumeration fails (e.g., permission denied)
        out.emplace_back("/data/system/users/0");
        return out;
    }
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        // Only directories with all-numeric names
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

// Convenience holder for dynamic BIND paths (avoids c_str() lifetime issues).
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
