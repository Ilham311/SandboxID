// gaid_persistence.hpp
//
// Persist the generated GAID UUID inside IDENTITY_FILE so it stays constant
// per identity generation (same GAID until user regenerates identity).
// Anti-fraud SDK cek stability: kalau UUID berubah setiap boot mereka anggap
// device rotate = fraud. Kalau UUID stabil untuk lifetime install → wajar.
//
// Format identity file baru (backward-compatible — kunci lama tetap terbaca):
//   ANDROID_ID=a375093d47cee211
//   GAID_UUID=8f14e45f-ceea-467a-9575-d0fab84bfa2b
//   ...
//
// Kalau file lama belum punya GAID_UUID (identity generation sebelum fix),
// generator akan buat baru, tulis balik ke file, dan pakai konsisten mulai boot itu.

#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include "secure_xml_gaid.hpp"

namespace ttfix {

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
// `identity_path` is the ternak_tt IDENTITY_FILE (e.g. /data/adb/ternak_tt/identity).
inline std::string get_or_create_gaid(const std::string& identity_path) {
    auto kv = read_identity(identity_path);
    auto it = kv.find("GAID_UUID");
    if (it != kv.end() && it->second.size() == 36) return it->second;
    std::string uuid = gen_gaid_uuid();
    kv["GAID_UUID"] = uuid;
    write_identity(identity_path, kv);
    return uuid;
}

// Convenience for regeneration path (`gen_identity` in ternak-tt.cpp): FORCE
// a fresh GAID UUID together with a new device. Call this whenever a fresh
// identity is generated so users get a clean fingerprint set.
inline std::string regen_gaid(const std::string& identity_path) {
    auto kv = read_identity(identity_path);
    std::string uuid = gen_gaid_uuid();
    kv["GAID_UUID"] = uuid;
    write_identity(identity_path, kv);
    return uuid;
}

} // namespace ttfix
