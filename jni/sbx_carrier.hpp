#pragma once

#include <string>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

namespace sbxcarrier {

inline std::string trim_ws(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n')) --e;
    return s.substr(b, e - b);
}

struct CarrierSel {
    bool        valid   = false;
    std::string name;
    std::string mcc;
    std::string mnc;
    std::string iso;
    std::string carrier_id;
    bool        phantom = false;
};

inline bool decimal_width(std::string_view value, size_t minimum,
                          size_t maximum) {
    if (value.size() < minimum || value.size() > maximum) return false;
    for (char c : value)
        if (c < '0' || c > '9') return false;
    return true;
}

inline bool valid_name(std::string_view value) {
    if (value.empty() || value.size() > 64 || value.front() == ' ' ||
        value.back() == ' ')
        return false;
    for (unsigned char c : value)
        if (c < 0x20 || c > 0x7e) return false;
    return true;
}

inline bool valid_iso(std::string_view value) {
    if (value.empty()) return true;
    return value.size() == 2 && value[0] >= 'a' && value[0] <= 'z' &&
           value[1] >= 'a' && value[1] <= 'z';
}

inline CarrierSel parse_carrier_conf(const std::string& raw) {
    CarrierSel s;
    if (raw.size() > 4096 || raw.find('\0') != std::string::npos) return s;
    std::set<std::string> seen;
    bool malformed = false;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        std::string t = trim_ws(line);
        if (t.empty() || t[0] == '#') continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) {
            malformed = true;
            continue;
        }
        std::string k = trim_ws(t.substr(0, eq));
        std::string v = trim_ws(t.substr(eq + 1));
        if (!seen.emplace(k).second) malformed = true;
        if      (k == "NAME")       s.name = v;
        else if (k == "MCC")        s.mcc  = v;
        else if (k == "MNC")        s.mnc  = v;
        else if (k == "ISO")        s.iso  = v;
        else if (k == "CARRIER_ID") s.carrier_id = v;
        else if (k == "PHANTOM") {
            if (v != "0" && v != "1") malformed = true;
            s.phantom = (v == "1");
        } else {
            malformed = true;
        }
    }
    s.valid = !malformed && valid_name(s.name) && decimal_width(s.mcc, 3, 3) &&
              decimal_width(s.mnc, 2, 3) && valid_iso(s.iso) &&
              (s.carrier_id.empty() || decimal_width(s.carrier_id, 1, 10));
    return s;
}

inline bool apply_carrier(std::map<std::string, std::string>& kv, const CarrierSel& s) {
    auto erase_all = [&] {
        kv.erase("GSM_OPERATOR_NUMERIC"); kv.erase("GSM_OPERATOR_ALPHA");
        kv.erase("GSM_OPERATOR_ISO");     kv.erase("GSM_SIM_STATE");
        kv.erase("GSM_CARRIER_ID");
    };
    if (!s.valid) { erase_all(); return false; }
    kv["GSM_OPERATOR_NUMERIC"] = s.mcc + s.mnc;
    kv["GSM_OPERATOR_ALPHA"]   = s.name;
    if (!s.iso.empty()) kv["GSM_OPERATOR_ISO"] = s.iso; else kv.erase("GSM_OPERATOR_ISO");
    if (!s.carrier_id.empty()) kv["GSM_CARRIER_ID"] = s.carrier_id; else kv.erase("GSM_CARRIER_ID");
    if (s.phantom)      kv["GSM_SIM_STATE"]    = "LOADED"; else kv.erase("GSM_SIM_STATE");
    return true;
}

}
