#ifndef SBX_IDENT_SYNTH_HPP
#define SBX_IDENT_SYNTH_HPP
// Deterministic synthesis of telephony / DRM identifiers from a persona seed.
//
// These identifiers (IMEI, IMSI, ICCID, MEID, Widevine deviceUniqueId) are served
// to apps ONLY through the L3 LSPlant hooks: they are delivered over Binder by the
// telephony/DRM system services, so the property-based layers (L2 SystemProperties,
// L7 typed getters, L9 libc __system_property_*) can never reach them.
//
// Because everything here is a pure function of `seed`, and main.cpp derives that
// seed from fnv1a(FINGERPRINT|SERIAL|ANDROID_ID) — all of which the identity blob
// rotates on every action.sh run — a fresh persona yields a fresh, internally
// consistent set of telephony identifiers with no extra plumbing through the shell
// generators (autopif.sh) or the C++ CLI (derive_identity).
//
// Host-testable: depends only on the header-only sbxnr:: seed primitives.
#include <string>
#include <cstdint>
#include "sbx_native_read.hpp"   // sbxnr::splitmix64 / fnv1a / hex_from_seed / mac_from_seed

namespace sbxid {

// --- Luhn (mod-10) -----------------------------------------------------------
inline char luhn_check_digit(const std::string& payload) {
    int sum = 0; bool dbl = true;            // rightmost payload digit is doubled
    for (int i = (int)payload.size() - 1; i >= 0; --i) {
        int n = payload[i] - '0';
        if (dbl) { n *= 2; if (n > 9) n -= 9; }
        sum += n; dbl = !dbl;
    }
    return (char)('0' + (10 - (sum % 10)) % 10);
}
inline bool luhn_valid(const std::string& full) {
    if (full.empty()) return false;
    int sum = 0; bool dbl = false;           // rightmost digit is the check digit
    for (int i = (int)full.size() - 1; i >= 0; --i) {
        if (full[i] < '0' || full[i] > '9') return false;
        int n = full[i] - '0';
        if (dbl) { n *= 2; if (n > 9) n -= 9; }
        sum += n; dbl = !dbl;
    }
    return sum % 10 == 0;
}

inline void append_digits(std::string& d, uint64_t& s, int count) {
    for (int i = 0; i < count; ++i) d += (char)('0' + (int)(sbxnr::splitmix64(s) % 10));
}

// --- IMEI: TAC(8) + SNR(6) + Luhn(1) = 15 -----------------------------------
// Reporting Body Identifier 35 (BABT) is the prefix most widely reported by
// Android handsets; these are plausible RBI-35 TAC-8 prefixes (not model-exact).
inline std::string synth_imei(uint64_t seed) {
    static const char* TAC[] = {
        "35161511","35316010","35404911","35847313",
        "35692211","35876554","35291612","35438110",
    };
    uint64_t s = seed ^ 0x494D4549ULL;                   // "IMEI"
    std::string d = TAC[sbxnr::splitmix64(s) % (sizeof(TAC)/sizeof(TAC[0]))];
    append_digits(d, s, 6);                              // SNR
    d += luhn_check_digit(d);                            // 15th digit
    return d;
}

// --- IMSI: MCC+MNC (from operator numeric) + MSIN, total 15, no check --------
inline std::string synth_imsi(uint64_t seed, std::string mccmnc) {
    if (mccmnc.size() < 5 || mccmnc.size() > 6) mccmnc = "51010"; // Telkomsel fallback
    for (char c : mccmnc) if (c < '0' || c > '9') { mccmnc = "51010"; break; }
    uint64_t s = seed ^ 0x494D5349ULL;                   // "IMSI"
    std::string d = mccmnc;
    append_digits(d, s, (int)(15 - mccmnc.size()));
    return d;
}

// ITU-T E.118 telephone country code for the SIM's MCC (best-effort; default 01).
inline const char* iccid_cc_for_mcc(const std::string& mccmnc) {
    if (mccmnc.rfind("510", 0) == 0) return "62";        // Indonesia
    if (mccmnc.rfind("310", 0) == 0 || mccmnc.rfind("311", 0) == 0) return "1"; // USA
    if (mccmnc.rfind("505", 0) == 0) return "61";        // Australia
    if (mccmnc.rfind("262", 0) == 0) return "49";        // Germany
    if (mccmnc.rfind("234", 0) == 0 || mccmnc.rfind("235", 0) == 0) return "44"; // UK
    return "01";
}

// --- ICCID: 89 + CC + issuer + account + Luhn, 19 digits ---------------------
inline std::string synth_iccid(uint64_t seed, const std::string& mccmnc) {
    uint64_t s = seed ^ 0x49434349ULL;                   // "ICCI"
    std::string d = "89";                                // Major Industry Id (telecom)
    d += iccid_cc_for_mcc(mccmnc);                        // country (1-2 digits)
    append_digits(d, s, (int)(18 - d.size()));            // issuer + account -> 18 payload
    d += luhn_check_digit(d);                             // -> 19 digits
    return d;
}

// --- MEID: 14 hex uppercase (CDMA device id, getMeid) ------------------------
// The leading hex digit is forced into A..F: the MEID numbering reserves the
// A0-FF "RR" region so a hex MEID never collides with the older decimal
// ESN/IMEI space. A MEID whose first nibble is 0-9 is malformed and fails
// fingerprint-SDK validators.
inline std::string synth_meid(uint64_t seed) {
    std::string h = sbxnr::hex_from_seed(seed ^ 0x4D454944ULL, 7); // 14 hex chars
    for (char& c : h) if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
    if (!h.empty()) {
        char c0 = h[0];
        int v = (c0 >= '0' && c0 <= '9') ? c0 - '0'
              : (c0 >= 'A' && c0 <= 'F') ? c0 - 'A' + 10 : 0;
        if (v < 0xA) h[0] = (char)('A' + (v % 6));    // 0-9 -> A-F, deterministic
    }
    return h;
}

// --- Widevine deviceUniqueId: 32 bytes -> 64 hex -----------------------------
inline std::string synth_widevine_hex(uint64_t seed) {
    return sbxnr::hex_from_seed(seed ^ 0x57565F4944ULL, 32); // "WV_ID"
}

// --- GSF ID (Google Services Framework "android_id") -------------------------
// The Gservices android_id is a signed 64-bit long surfaced as a decimal string
// through content://com.google.android.gsf.gservices. FingerprintJS-style device
// scores rank it ABOVE the Settings ANDROID_ID and the MediaDrm id, so it must
// rotate with the persona too. We clear the top bit (>> 1) so the value is a
// positive 63-bit long — a negative or zero GSF id is treated as "not yet
// registered" by callers and would itself be a tell.
inline std::string synth_gsf_id(uint64_t seed) {
    uint64_t s = seed ^ 0x4753464944ULL;         // "GSFID"
    uint64_t v = sbxnr::splitmix64(s) >> 1;       // 63-bit, always non-negative as a signed long
    if (v == 0) v = 1;                            // 0 == "no GSF id" — never emit it
    return std::to_string(v);
}

// Bundle so callers synthesize once.
struct SynthIds {
    std::string imei, imsi, iccid, meid, widevine_hex;
};
inline SynthIds synth_all(uint64_t seed, const std::string& mccmnc) {
    SynthIds o;
    o.imei         = synth_imei(seed);
    o.imsi         = synth_imsi(seed, mccmnc);
    o.iccid        = synth_iccid(seed, mccmnc);
    o.meid         = synth_meid(seed);
    o.widevine_hex = synth_widevine_hex(seed);
    return o;
}

} // namespace sbxid
#endif // SBX_IDENT_SYNTH_HPP
