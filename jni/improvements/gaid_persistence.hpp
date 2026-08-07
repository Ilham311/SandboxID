

#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include "secure_xml_gaid.hpp"

namespace ttfix {

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

inline std::string get_or_create_gaid(const std::string& identity_path) {
    auto kv = read_identity(identity_path);
    auto it = kv.find("GAID_UUID");
    if (it != kv.end() && it->second.size() == 36) return it->second;
    std::string uuid = gen_gaid_uuid();
    kv["GAID_UUID"] = uuid;
    write_identity(identity_path, kv);
    return uuid;
}

inline std::string regen_gaid(const std::string& identity_path) {
    auto kv = read_identity(identity_path);
    std::string uuid = gen_gaid_uuid();
    kv["GAID_UUID"] = uuid;
    write_identity(identity_path, kv);
    return uuid;
}

}
