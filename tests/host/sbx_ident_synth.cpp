// Host verification for jni/sbx_ident_synth.hpp — the identifiers the L3
// telephony/DRM hooks serve.
//
// Why this suite exists. Every value here is synthesized from the persona seed,
// so a bug does not crash: it silently serves an identifier that contradicts
// the rest of the identity. That is exactly what happened on device. The
// operator string was empty (no carrier.conf, and the module no longer invents
// one), synth_imsi() substituted a hardcoded "51010", and Grab recorded
// `"mnc":"11"` — the REAL MNC, read from a radio surface the module does not
// hook — beside a spoofed Telkomsel name in the same event stream. Nothing in
// CI could see it, because nothing tested this header at all.
//
// The rule the checks below pin: a synthesized identifier is either internally
// consistent with the operator it was built from, or it is EMPTY so the caller
// falls through to the real value. Never a plausible-looking third thing.
//
// Pure header test: no module TU, no device, no Zygisk surface.
#include "sbx_ident_synth.hpp"

#include <cstdio>
#include <string>

static int fails = 0, checks = 0;

#define CHECK(cond, msg) do { \
    ++checks; \
    if (!(cond)) { ++fails; printf("FAIL: %s\n", msg); } \
    else printf("ok:   %s\n", msg); \
} while (0)

// Luhn over a digit string, used to verify the check digit synth_imei appends.
static bool luhn_ok(const std::string& d) {
    int sum = 0;
    bool dbl = false;
    for (int i = (int)d.size() - 1; i >= 0; --i) {
        int n = d[i] - '0';
        if (dbl) { n *= 2; if (n > 9) n -= 9; }
        sum += n;
        dbl = !dbl;
    }
    return sum % 10 == 0;
}

static bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

int main() {
    using namespace sbxid;

    const uint64_t seed = 0x1234567890abcdefULL;

    // ---- IMEI: always synthesizable, independent of the SIM ----------------
    {
        std::string a = synth_imei(seed);
        std::string b = synth_imei(seed);
        std::string c = synth_imei(seed ^ 1);
        CHECK(a.size() == 15, "imei is 15 digits");
        CHECK(all_digits(a), "imei is all digits");
        CHECK(luhn_ok(a), "imei check digit is valid Luhn");
        CHECK(a == b, "imei deterministic for one seed");
        CHECK(a != c, "imei differs for a different seed");
        // RBI 35 is the reporting body every Android handset in the wild uses.
        CHECK(a.compare(0, 2, "35") == 0, "imei uses an RBI-35 TAC prefix");
    }

    // ---- IMSI: consistent with the operator, or empty ----------------------
    {
        const std::string imsi = synth_imsi(seed, "51011");   // XL, the test SIM
        CHECK(imsi.size() == 15, "imsi is 15 digits");
        CHECK(all_digits(imsi), "imsi is all digits");
        CHECK(imsi.compare(0, 5, "51011") == 0, "imsi carries the operator's MCC+MNC");
        CHECK(imsi == synth_imsi(seed, "51011"), "imsi deterministic for one seed");
        CHECK(imsi != synth_imsi(seed, "51010"), "imsi follows the operator, not a constant");

        // A 3-digit MNC is a real allocation (e.g. 310-260 T-Mobile); the total
        // stays 15 either way.
        const std::string imsi3 = synth_imsi(seed, "310260");
        CHECK(imsi3.size() == 15, "imsi with a 3-digit MNC is still 15 digits");
        CHECK(imsi3.compare(0, 6, "310260") == 0, "imsi carries a 3-digit MNC");

        // The regression this suite was written for: an unknown operator must
        // yield NOTHING, so the hook falls through to the real value. It used
        // to yield a Telkomsel-prefixed IMSI that contradicted the radio.
        CHECK(synth_imsi(seed, "").empty(), "imsi is empty for an unknown operator");
        CHECK(synth_imsi(seed, "510").empty(), "imsi is empty for a truncated operator");
        CHECK(synth_imsi(seed, "5101111").empty(), "imsi is empty for an over-long operator");
        CHECK(synth_imsi(seed, "51011,").empty(),
              "imsi is empty for the per-slot comma the GSM props carry");
        CHECK(synth_imsi(seed, "51O11").empty(), "imsi is empty for a non-numeric operator");
    }

    // ---- ICCID: country code follows the MCC, or empty ---------------------
    {
        const std::string iccid = synth_iccid(seed, "51011");
        CHECK(iccid.size() == 19, "iccid is 19 digits");
        CHECK(all_digits(iccid), "iccid is all digits");
        CHECK(luhn_ok(iccid), "iccid check digit is valid Luhn");
        CHECK(iccid.compare(0, 4, "8962") == 0, "iccid starts 89 + Indonesia (62) for MCC 510");
        CHECK(iccid == synth_iccid(seed, "51011"), "iccid deterministic for one seed");

        CHECK(synth_iccid(seed, "310260").compare(0, 3, "891") == 0,
              "iccid uses country code 1 for MCC 310");
        CHECK(synth_iccid(seed, "26201").compare(0, 3, "894") == 0,
              "iccid uses country code 49 for MCC 262");

        CHECK(synth_iccid(seed, "").empty(), "iccid is empty for an unknown operator");
        CHECK(synth_iccid(seed, "51011,").empty(), "iccid is empty for the per-slot comma");
    }

    // ---- MEID / Widevine: no operator dependency --------------------------
    {
        const std::string meid = synth_meid(seed);
        CHECK(meid.size() == 14, "meid is 14 hex chars");
        CHECK(meid == synth_meid(seed), "meid deterministic for one seed");
        CHECK(meid != synth_meid(seed ^ 1), "meid differs for a different seed");
        bool upper = true;
        for (char c : meid) if (c >= 'a' && c <= 'f') upper = false;
        CHECK(upper, "meid is uppercase hex");

        const std::string wv = synth_widevine_hex(seed);
        CHECK(wv.size() == 64, "widevine id is 32 bytes of hex");
        CHECK(wv == synth_widevine_hex(seed), "widevine deterministic for one seed");
        CHECK(wv != synth_widevine_hex(seed ^ 1), "widevine differs for a different seed");
    }

    // ---- synth_all: one bundle, one operator, no cross-field drift --------
    {
        const SynthIds a = synth_all(seed, "51011");
        CHECK(!a.imei.empty() && !a.imsi.empty() && !a.iccid.empty(),
              "synth_all fills every identifier for a known operator");
        CHECK(a.imsi.compare(0, 5, "51011") == 0, "synth_all imsi matches its operator");
        CHECK(a.iccid.compare(0, 4, "8962") == 0, "synth_all iccid matches its operator");

        const SynthIds b = synth_all(seed, "");
        CHECK(!b.imei.empty(), "synth_all still synthesizes the imei with no operator");
        CHECK(b.imsi.empty(), "synth_all leaves the imsi empty with no operator");
        CHECK(b.iccid.empty(), "synth_all leaves the iccid empty with no operator");

        // Rotating the persona seed must rotate the SIM-bound ids too, or a
        // reset would hand the app the same SIM it saw before.
        const SynthIds c = synth_all(seed ^ 0xff, "51011");
        CHECK(c.imsi != a.imsi, "imsi rotates with the persona seed");
        CHECK(c.iccid != a.iccid, "iccid rotates with the persona seed");
        CHECK(c.imei != a.imei, "imei rotates with the persona seed");
    }

    // ---- MCC / MNC split: the halves several getters expose separately -----
    {
        CHECK(operator_mcc("51011") == "510", "mcc is the first three digits");
        CHECK(operator_mnc("51011") == "11", "mnc is the 2-digit remainder");
        CHECK(operator_mcc("310260") == "310", "mcc of a 3-digit-MNC operator");
        CHECK(operator_mnc("310260") == "260", "mnc of a 3-digit-MNC operator");

        // Unknown operator: both halves empty, so each getter falls through to
        // the real value. Returning "" for one and something for the other
        // would be the contradiction this whole change is about.
        CHECK(operator_mcc("").empty(), "mcc is empty for an unknown operator");
        CHECK(operator_mnc("").empty(), "mnc is empty for an unknown operator");
        CHECK(operator_mcc("51011,").empty(), "mcc is empty for the per-slot comma");
        CHECK(operator_mnc("51011,").empty(), "mnc is empty for the per-slot comma");
        CHECK(operator_mcc("51O11").empty(), "mcc is empty for a non-numeric operator");
        CHECK(operator_mnc("5101111").empty(), "mnc is empty for an over-long operator");

        // The halves must reassemble to the original, or the two getters would
        // describe different networks.
        const std::string num = "51011";
        CHECK(operator_mcc(num) + operator_mnc(num) == num,
              "mcc + mnc reassembles the operator numeric");
    }

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
