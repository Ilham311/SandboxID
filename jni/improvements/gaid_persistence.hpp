// gaid_persistence.hpp
// v2.1.2: renamed namespace ttfix:: -> tt::, uses tt::uuid_v4 directly.
//
// Persist the generated GAID UUID inside IDENTITY_FILE so it stays constant
// per identity generation (same GAID until user regenerates identity).
//
// Format identity file (backward-compatible):
//   ANDROID_ID=a375093d47cee211
//   GAID_UUID=8f14e45f-ceea-467a-9575-d0fab84bfa2b

#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include "random_util.hpp"  // tt::uuid_v4

namespace tt {

// Very small kv parser (KEY=VALUE per line, '#' comment).
inline std::unordered_map<std::string, std::string>
read_identity(const std::string& path) {
    std::unordered_map<std::string, std::string> kv;
    std::ifstream f(path);
    if (!f.good()) return kv;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

inline bool write_identity(const std::string& path,
                           const std::unordered_map<std::string, std::string>& kv) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.good()) return false;
    for (const auto& p : kv) f << p.first << '=' << p.second << '\n';
    return f.good();
}

// Returns the GAID UUID for this identity, creating one if the file lacks it.
inline std::string get_or_create_gaid(const std::string& identity_path) {
    auto kv = read_identity(identity_path);
    auto it = kv.find("GAID_UUID");
    if (it != kv.end() && it->second.size() == 36) return it->second;
    std::string uuid = uuid_v4();
    kv["GAID_UUID"] = uuid;
    write_identity(identity_path, kv);
    return uuid;
}

// Force a fresh GAID together with new identity.
inline std::string regen_gaid(const std::string& identity_path) {
    auto kv = read_identity(identity_path);
    std::string uuid = uuid_v4();
    kv["GAID_UUID"] = uuid;
    write_identity(identity_path, kv);
    return uuid;
}

}  // namespace tt
