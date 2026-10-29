#ifndef SBX_IDENT_SYNTH_HPP
#define SBX_IDENT_SYNTH_HPP
// Deterministic synthesis of telephony / DRM identifiers from a persona seed.
//
// These identifiers (IMEI, IMSI, ICCID, MEID, Widevine deviceUniqueId) are served
// to apps ONLY through the L3 LSPlant hooks: they are delivered over Binder by the
// telephony/DRM system services, so the property-based layers (L2 SystemProperties,
// L7 typed getters, L9 libc __system_property_*) can never reach them.
//
// Because everything here is a pure function of `seed`, and module.cpp derives that
// seed from fnv1a(FINGERPRINT|SERIAL|ANDROID_ID) — all of which the identity blob
// rotates on every action.sh run — a fresh persona yields a fresh, internally
// consistent set of telephony identifiers with no extra plumbing through the shell
// generators (autopif.sh) or the C++ CLI (derive_identity).
//
// Host-testable: depends only on the header-only sbxnr:: seed primitives.
#include <string>
#include <cstdint>
#include "native_read.hpp"   // sbxnr::splitmix64 / fnv1a / hex_from_seed / mac_from_seed

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
// The MCC+MNC prefix is NOT invented. It used to fall back to "51010"
// (Telkomsel) whenever the operator string was missing or malformed, which
// produced an IMSI naming a network the device was not attached to - the same
// request then carried the real MNC read from the radio, so the two halves of
// the identity disagreed. An empty result is the honest answer: the caller
// leaves the getter unspoofed and the real (self-consistent) value is served.
inline std::string synth_imsi(uint64_t seed, std::string mccmnc) {
    if (mccmnc.size() < 5 || mccmnc.size() > 6) return std::string();
    for (char c : mccmnc) if (c < '0' || c > '9') return std::string();
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
// Empty when the operator is unknown, for the same reason as synth_imsi(): the
// country-code half is derived from the MCC, so without one the result would
// describe a SIM from a country the device is not in. A device with no SIM
// reports null from getSimSerialNumber(), and letting the hook fall through to
// the original reproduces exactly that.
inline std::string synth_iccid(uint64_t seed, const std::string& mccmnc) {
    if (mccmnc.size() < 5 || mccmnc.size() > 6) return std::string();
    for (char c : mccmnc) if (c < '0' || c > '9') return std::string();
    uint64_t s = seed ^ 0x49434349ULL;                   // "ICCI"
    std::string d = "89";                                // Major Industry Id (telecom)
    d += iccid_cc_for_mcc(mccmnc);                        // country (1-2 digits)
    append_digits(d, s, (int)(18 - d.size()));            // issuer + account -> 18 payload
    d += luhn_check_digit(d);                             // -> 19 digits
    return d;
}

// --- MEID: 14 hex uppercase (CDMA device id, getMeid) ------------------------
inline std::string synth_meid(uint64_t seed) {
    std::string h = sbxnr::hex_from_seed(seed ^ 0x4D454944ULL, 7); // 14 hex chars
    for (char& c : h) if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
    return h;
}

// --- Widevine deviceUniqueId: 32 bytes -> 64 hex -----------------------------
inline std::string synth_widevine_hex(uint64_t seed) {
    return sbxnr::hex_from_seed(seed ^ 0x57565F4944ULL, 32); // "WV_ID"
}

// --- MCC / MNC split of an operator numeric --------------------------------
// The joined "MCC+MNC" string is what gsm.operator.numeric carries, but several
// framework getters expose the halves separately (CellIdentityLte.getMnc(),
// SubscriptionInfo.getMcc(), ...). Those are a different API against a
// different object than TelephonyManager.getNetworkOperator(), so hooking the
// joined form does not cover them - and an app that reads the halves recorded
// the REAL MNC while every other carrier field on the same event was spoofed.
//
// MCC is always three digits; the MNC is the remainder, 2 or 3 digits depending
// on the allocation. An unrecognised string yields empty halves, so the caller
// falls through to the real value instead of inventing one.
inline std::string operator_mcc(const std::string& numeric) {
    if (numeric.size() < 5 || numeric.size() > 6) return std::string();
    for (char c : numeric) if (c < '0' || c > '9') return std::string();
    return numeric.substr(0, 3);
}

inline std::string operator_mnc(const std::string& numeric) {
    if (numeric.size() < 5 || numeric.size() > 6) return std::string();
    for (char c : numeric) if (c < '0' || c > '9') return std::string();
    return numeric.substr(3);
}

// --- MAC selection: the persona's own value, or a seed-derived fallback -----
// Both MAC surfaces (WifiInfo.getMacAddress, BluetoothAdapter.getAddress) and
// the bluetooth_address row of Settings.Secure must report the SAME address the
// persona declares, or the app sees two different phones in one session.
//
// The bug this exists to prevent: install_all() honoured HookValues::wifi_mac
// but built the Bluetooth address unconditionally from the seed, so a persona
// declaring BLUETOOTH_ADDR=02:91:1a:ba:67:2a still reported
// 02:84:86:7D:02:DC from getAddress() - and Settings.Secure's real
// bluetooth_address (8C:AA:CE:2C:19:D5) was never hooked at all, so the app had
// THREE different Bluetooth addresses available to it.
//
// The fallback is only for a persona that declares no address - or declares one
// that is not a MAC at all. It stays locally administered (mac_from_seed forces
// the 02 prefix), so it can never collide with a real vendor OUI. A malformed
// persona value is treated exactly like an absent one: serving "not-a-mac" from
// getAddress() is an obvious tell, and serving the REAL address is worse.
//
// `upper` is the platform's own convention, not a preference: Android reports
// BluetoothAdapter.getAddress() and Settings.Secure's bluetooth_address in
// uppercase hex, while /sys/class/net/wlan0/address and WifiInfo.getMacAddress()
// are lowercase. Keeping each surface in its native case means the spoofed value
// is indistinguishable in FORM from the real one it replaces.
inline std::string pick_mac(const std::string& persona, uint64_t seed,
                            uint64_t salt, bool upper) {
    std::string m = sbxnr::is_valid_mac(persona) ? persona
                                                 : sbxnr::mac_from_seed(seed ^ salt);
    if (upper) for (char& c : m) if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
    return m;
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
