#pragma once

#include <string>
#include <map>
#include <sstream>

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
    std::string carrier_id;   // Android canonical carrier id (getSimCarrierId); empty = unset/UNKNOWN
    bool        phantom = false;
};

inline CarrierSel parse_carrier_conf(const std::string& raw) {
    CarrierSel s;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        std::string t = trim_ws(line);
        if (t.empty() || t[0] == '#') continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_ws(t.substr(0, eq));
        std::string v = trim_ws(t.substr(eq + 1));
        if      (k == "NAME")       s.name = v;
        else if (k == "MCC")        s.mcc  = v;
        else if (k == "MNC")        s.mnc  = v;
        else if (k == "ISO")        s.iso  = v;
        else if (k == "CARRIER_ID") s.carrier_id = v;
        else if (k == "PHANTOM")    s.phantom = (v == "1");
    }
    s.valid = !s.name.empty() && !s.mcc.empty() && !s.mnc.empty();
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
