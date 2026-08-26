#pragma once
// sbx_carrier.hpp — pure carrier/SIM selection parsing, split out of
// sandboxid.cpp so it can be unit-tested on the host (see tests/carrier_test.cpp).
//
// carrier.conf is a tiny KV file written by the `carrier` command / WebUI:
//     NAME=AT&T
//     MCC=310
//     MNC=410
//     ISO=us
//     PHANTOM=0|1
//
// This header turns that text into the four identity.prop keys the property
// hook consumes, with the exact "owns the keys" contract merge_carrier relies
// on: a valid selection SETS the keys; anything else ERASES them so the module
// falls back to the built-in default (VAL_DEFAULTS). Keeping this pure (string
// in, map mutation out — no file I/O, no Identity, no JNI) is what makes it
// testable and keeps the shell and native paths in agreement.

#include <string>
#include <map>
#include <sstream>

namespace sbxcarrier {

// Local trim so the header is self-contained (avoids depending on sandboxid.cpp).
inline std::string trim_ws(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n')) --e;
    return s.substr(b, e - b);
}

struct CarrierSel {
    bool        valid   = false;   // name+mcc+mnc all present
    std::string name;
    std::string mcc;
    std::string mnc;
    std::string iso;               // may be empty
    bool        phantom = false;   // PHANTOM=1 -> force SIM present
};

// Parse carrier.conf text. Ignores blank/`#` lines; splits on the first '='.
// Whitespace is trimmed from every field. `valid` is true only when name, mcc
// and mnc are all non-empty (an incomplete file must NOT produce a half-built,
// malformed operator numeric).
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
        if      (k == "NAME")    s.name = v;
        else if (k == "MCC")     s.mcc  = v;
        else if (k == "MNC")     s.mnc  = v;
        else if (k == "ISO")     s.iso  = v;
        else if (k == "PHANTOM") s.phantom = (v == "1");
    }
    s.valid = !s.name.empty() && !s.mcc.empty() && !s.mnc.empty();
    return s;
}

// Apply a parsed selection onto an identity key/value map, OWNING the four
// carrier keys: set them for a valid selection, erase them otherwise. Erasing
// (rather than blanking) lets serialize() drop the keys so the hook's val()
// falls through to VAL_DEFAULTS. Returns true when a carrier was applied.
inline bool apply_carrier(std::map<std::string, std::string>& kv, const CarrierSel& s) {
    auto erase_all = [&] {
        kv.erase("GSM_OPERATOR_NUMERIC"); kv.erase("GSM_OPERATOR_ALPHA");
        kv.erase("GSM_OPERATOR_ISO");     kv.erase("GSM_SIM_STATE");
    };
    if (!s.valid) { erase_all(); return false; }
    kv["GSM_OPERATOR_NUMERIC"] = s.mcc + s.mnc;   // MCC+MNC, widths preserved
    kv["GSM_OPERATOR_ALPHA"]   = s.name;
    if (!s.iso.empty()) kv["GSM_OPERATOR_ISO"] = s.iso; else kv.erase("GSM_OPERATOR_ISO");
    if (s.phantom)      kv["GSM_SIM_STATE"]    = "LOADED"; else kv.erase("GSM_SIM_STATE");
    return true;
}

}  // namespace sbxcarrier
