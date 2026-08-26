
#include "../jni/sbx_carrier.hpp"

#include <cstdio>
#include <string>
#include <map>

using namespace sbxcarrier;

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond, msg) do {                                            \
    ++g_checks;                                                          \
    if (!(cond)) { ++g_fails; std::printf("FAIL: %s  (%s:%d)\n",         \
                                          (msg), __FILE__, __LINE__); }  \
} while (0)

static bool has(const std::map<std::string, std::string>& m, const std::string& k) {
    return m.find(k) != m.end();
}
static std::string get(const std::map<std::string, std::string>& m, const std::string& k) {
    auto it = m.find(k);
    return it == m.end() ? std::string("<absent>") : it->second;
}

static void test_parse_full() {
    CarrierSel s = parse_carrier_conf("NAME=AT&T\nMCC=310\nMNC=410\nISO=us\nCARRIER_ID=1187\nPHANTOM=1\n");
    CHECK(s.valid, "full conf is valid");
    CHECK(s.name == "AT&T", "name parsed");
    CHECK(s.mcc == "310", "mcc parsed");
    CHECK(s.mnc == "410", "mnc parsed");
    CHECK(s.iso == "us", "iso parsed");
    CHECK(s.carrier_id == "1187", "carrier_id parsed");
    CHECK(s.phantom, "phantom=1 -> true");

    std::map<std::string, std::string> kv;
    bool applied = apply_carrier(kv, s);
    CHECK(applied, "apply returns true for valid");
    CHECK(get(kv, "GSM_OPERATOR_NUMERIC") == "310410", "numeric = mcc+mnc");
    CHECK(get(kv, "GSM_OPERATOR_ALPHA") == "AT&T", "alpha = name");
    CHECK(get(kv, "GSM_OPERATOR_ISO") == "us", "iso set");
    CHECK(get(kv, "GSM_CARRIER_ID") == "1187", "carrier_id set");
    CHECK(get(kv, "GSM_SIM_STATE") == "LOADED", "phantom -> LOADED");
}

// carrier_id is optional: present sets GSM_CARRIER_ID, absent/blank erases it
// (mirrors ISO handling). Blank is the truthful value for operators not in
// Google's carrier DB (a real device reports UNKNOWN_CARRIER_ID = -1 there).
static void test_carrier_id_optional() {
    // Telkomsel with its real AOSP carrier id 787.
    CarrierSel s = parse_carrier_conf("NAME=Telkomsel\nMCC=510\nMNC=10\nISO=id\nCARRIER_ID=787\nPHANTOM=0\n");
    CHECK(s.carrier_id == "787", "telkomsel carrier_id 787 parsed");
    std::map<std::string, std::string> kv;
    apply_carrier(kv, s);
    CHECK(get(kv, "GSM_CARRIER_ID") == "787", "telkomsel GSM_CARRIER_ID = 787");

    // No CARRIER_ID line -> key absent.
    CarrierSel none = parse_carrier_conf("NAME=Smartfren\nMCC=510\nMNC=09\nISO=id\nPHANTOM=0\n");
    CHECK(none.carrier_id.empty(), "missing carrier_id -> empty");
    std::map<std::string, std::string> kv2;
    kv2["GSM_CARRIER_ID"] = "999";  // stale
    apply_carrier(kv2, none);
    CHECK(!has(kv2, "GSM_CARRIER_ID"), "blank carrier_id erases the key");
}

static void test_name_with_spaces_no_phantom() {
    CarrierSel s = parse_carrier_conf("NAME=NTT DoCoMo\nMCC=440\nMNC=10\nISO=jp\nPHANTOM=0\n");
    CHECK(s.name == "NTT DoCoMo", "name with spaces preserved");
    CHECK(!s.phantom, "phantom=0 -> false");
    std::map<std::string, std::string> kv;
    apply_carrier(kv, s);
    CHECK(get(kv, "GSM_OPERATOR_NUMERIC") == "44010", "numeric jp");
    CHECK(get(kv, "GSM_OPERATOR_ALPHA") == "NTT DoCoMo", "alpha with space");
    CHECK(!has(kv, "GSM_SIM_STATE"), "no phantom -> sim state absent");
}

static void test_three_digit_mnc() {

    CarrierSel s = parse_carrier_conf("NAME=Telcel\nMCC=334\nMNC=020\nISO=mx\nPHANTOM=0\n");
    std::map<std::string, std::string> kv;
    apply_carrier(kv, s);
    CHECK(get(kv, "GSM_OPERATOR_NUMERIC") == "334020", "3-digit mnc width preserved");
}

static void test_empty_iso_erased() {
    CarrierSel s = parse_carrier_conf("NAME=Foo\nMCC=111\nMNC=22\nISO=\nPHANTOM=0\n");
    CHECK(s.valid, "valid without iso");
    std::map<std::string, std::string> kv;
    kv["GSM_OPERATOR_ISO"] = "stale";
    apply_carrier(kv, s);
    CHECK(!has(kv, "GSM_OPERATOR_ISO"), "empty iso erases the key");
}

static void test_invalid_empty_and_incomplete() {
    std::map<std::string, std::string> kv;

    kv["GSM_OPERATOR_NUMERIC"] = "51010";
    kv["GSM_OPERATOR_ALPHA"] = "Telkomsel";
    kv["GSM_OPERATOR_ISO"] = "id";
    kv["GSM_CARRIER_ID"] = "787";
    kv["GSM_SIM_STATE"] = "LOADED";
    kv["MODEL"] = "Pixel 8";

    CarrierSel empty = parse_carrier_conf("");
    CHECK(!empty.valid, "empty conf invalid");
    bool applied = apply_carrier(kv, empty);
    CHECK(!applied, "apply returns false for invalid");
    CHECK(!has(kv, "GSM_OPERATOR_NUMERIC"), "invalid erases numeric");
    CHECK(!has(kv, "GSM_OPERATOR_ALPHA"), "invalid erases alpha");
    CHECK(!has(kv, "GSM_OPERATOR_ISO"), "invalid erases iso");
    CHECK(!has(kv, "GSM_CARRIER_ID"), "invalid erases carrier_id");
    CHECK(!has(kv, "GSM_SIM_STATE"), "invalid erases sim state");
    CHECK(get(kv, "MODEL") == "Pixel 8", "unrelated key preserved");

    CarrierSel incomplete = parse_carrier_conf("NAME=Foo\nMCC=111\n");
    CHECK(!incomplete.valid, "missing mnc -> invalid");
}

static void test_comments_blanks_crlf_ws() {
    CarrierSel s = parse_carrier_conf(
        "# a comment\r\n"
        "\r\n"
        "  NAME = Vodafone \r\n"
        "MCC=234\r\n"
        "MNC=15\r\n"
        "# trailing comment\n"
        "ISO=gb\r\n"
        "PHANTOM=1\r\n");
    CHECK(s.name == "Vodafone", "whitespace around key/value trimmed");
    CHECK(s.mcc == "234" && s.mnc == "15", "crlf stripped from values");
    CHECK(s.iso == "gb", "iso after comment line");
    CHECK(s.phantom, "phantom parsed with crlf");
    CHECK(s.valid, "valid overall");
}

static void test_phantom_strictly_one() {
    CHECK(!parse_carrier_conf("NAME=A\nMCC=1\nMNC=2\nPHANTOM=true\n").phantom,
          "PHANTOM=true is not phantom (only \"1\")");
    CHECK(!parse_carrier_conf("NAME=A\nMCC=1\nMNC=2\nPHANTOM=2\n").phantom,
          "PHANTOM=2 is not phantom");
    CHECK(parse_carrier_conf("NAME=A\nMCC=1\nMNC=2\nPHANTOM=1\n").phantom,
          "PHANTOM=1 is phantom");
}

static void test_reapply_switches_cleanly() {
    std::map<std::string, std::string> kv;
    apply_carrier(kv, parse_carrier_conf("NAME=A\nMCC=310\nMNC=410\nISO=us\nCARRIER_ID=1187\nPHANTOM=1\n"));
    CHECK(get(kv, "GSM_SIM_STATE") == "LOADED", "first apply sets phantom");
    CHECK(get(kv, "GSM_CARRIER_ID") == "1187", "first apply sets carrier_id");

    apply_carrier(kv, parse_carrier_conf("NAME=B\nMCC=440\nMNC=20\nPHANTOM=0\n"));
    CHECK(get(kv, "GSM_OPERATOR_NUMERIC") == "44020", "numeric updated on re-apply");
    CHECK(get(kv, "GSM_OPERATOR_ALPHA") == "B", "alpha updated on re-apply");
    CHECK(!has(kv, "GSM_SIM_STATE"), "phantom cleared on re-apply");
    CHECK(!has(kv, "GSM_OPERATOR_ISO"), "iso cleared on re-apply");
    CHECK(!has(kv, "GSM_CARRIER_ID"), "carrier_id cleared on re-apply");
}

int main() {
    test_parse_full();
    test_carrier_id_optional();
    test_name_with_spaces_no_phantom();
    test_three_digit_mnc();
    test_empty_iso_erased();
    test_invalid_empty_and_incomplete();
    test_comments_blanks_crlf_ws();
    test_phantom_strictly_one();
    test_reapply_switches_cleanly();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
