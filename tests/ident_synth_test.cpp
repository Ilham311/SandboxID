
#include "../jni/sbx_ident_synth.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using namespace sbxid;

static int g_checks = 0;
static int g_fails  = 0;
#define CHECK(cond, msg) do {                                            \
    ++g_checks;                                                          \
    if (!(cond)) { ++g_fails; std::printf("FAIL: %s  (%s:%d)\n",         \
                                          (msg), __FILE__, __LINE__); }  \
} while (0)

static bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}
static bool all_hex_upper(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

int main() {
    uint64_t seed = sbxnr::fnv1a("google/husky/husky:14/AP1A.240505.004/11583682:user/release-keys"
                                 "|A1B2C3D4E5F60718|deadbeefcafef00d");

    CHECK(luhn_valid("35316010" "123456" + std::string(1, luhn_check_digit("35316010123456"))),
          "luhn_check_digit produces a valid full number");
    {
        std::string base = "35316010123456";
        char good = luhn_check_digit(base);
        char bad  = (char)('0' + (((good - '0') + 1) % 10));
        CHECK(!luhn_valid(base + std::string(1, bad)), "luhn_valid rejects a corrupted check digit");
    }
    CHECK(luhn_valid("79927398713"), "luhn_valid accepts the canonical test number 79927398713");

    std::string imei = synth_imei(seed);
    CHECK(imei.size() == 15, "IMEI is 15 digits");
    CHECK(all_digits(imei), "IMEI is all digits");
    CHECK(luhn_valid(imei), "IMEI passes Luhn");
    CHECK(imei.rfind("35", 0) == 0, "IMEI TAC uses RBI 35");
    CHECK(synth_imei(seed) == imei, "IMEI deterministic for same seed");
    CHECK(synth_imei(seed ^ 1ULL) != imei, "IMEI differs for a different seed");

    std::string imsi = synth_imsi(seed, "51010");
    CHECK(imsi.size() == 15, "IMSI is 15 digits");
    CHECK(all_digits(imsi), "IMSI is all digits");
    CHECK(imsi.rfind("51010", 0) == 0, "IMSI begins with the operator MCC+MNC");
    CHECK(synth_imsi(seed, "310260").rfind("310260", 0) == 0, "IMSI honors a 6-digit MNC");
    CHECK(synth_imsi(seed, "bogus").rfind("51010", 0) == 0, "IMSI falls back on a non-numeric operator");
    CHECK(synth_imsi(seed, "51010") == imsi, "IMSI deterministic");

    std::string iccid = synth_iccid(seed, "51010");
    CHECK(iccid.size() == 19, "ICCID is 19 digits");
    CHECK(all_digits(iccid), "ICCID is all digits");
    CHECK(iccid.rfind("89", 0) == 0, "ICCID begins with MII 89");
    CHECK(iccid.rfind("8962", 0) == 0, "ICCID country for MCC 510 is 62 (Indonesia)");
    CHECK(luhn_valid(iccid), "ICCID passes Luhn");
    CHECK(synth_iccid(seed, "51010") == iccid, "ICCID deterministic");

    std::string meid = synth_meid(seed);
    CHECK(meid.size() == 14, "MEID is 14 hex chars");
    CHECK(all_hex_upper(meid), "MEID is uppercase hex");
    CHECK(synth_meid(seed) == meid, "MEID deterministic");

    {
        bool all_af = true;
        for (uint64_t k = 0; k < 4096; ++k) {
            char c0 = synth_meid(seed ^ (k * 0x9E3779B97F4A7C15ULL))[0];
            if (c0 < 'A' || c0 > 'F') { all_af = false; break; }
        }
        CHECK(all_af, "MEID leading hex digit is A-F for all seeds");
    }

    std::string wv = synth_widevine_hex(seed);
    CHECK(wv.size() == 64, "Widevine id is 64 hex chars (32 bytes)");
    CHECK(synth_widevine_hex(seed) == wv, "Widevine id deterministic");
    CHECK(synth_widevine_hex(seed ^ 2ULL) != wv, "Widevine id differs for a different seed");

    SynthIds b = synth_all(seed, "51010");
    CHECK(b.imei == imei && b.imsi == imsi && b.iccid == iccid &&
          b.meid == meid && b.widevine_hex == wv, "synth_all matches individual functions");

    std::printf("ident_synth_test: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
