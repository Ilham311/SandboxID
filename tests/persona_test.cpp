#include "../jni/sbx_persona.hpp"
#include "../jni/sbx_identity.hpp"
#include "../jni/sbx_sha256.hpp"
#include "../jni/sbx_transaction.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

static int checks = 0;
static int failures = 0;

#define CHECK(condition, message) do {                                    \
    ++checks;                                                             \
    if (!(condition)) {                                                   \
        ++failures;                                                       \
        std::printf("FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
    }                                                                     \
} while (0)

static sbxpersona::Persona persona(const char* model, int sdk,
                                   const char* device = "husky") {
    sbxpersona::Persona value;
    value.model = model;
    value.device = device;
    value.product = device;
    value.board = device;
    value.platform = "zuma";
    value.sdk = sdk;
    value.release = sdk == 35 ? "15" : "14";
    value.id = sdk == 35 ? "AP3A.241105.008" : "AP1A.240505.004";
    value.incremental = sdk == 35 ? "12433045" : "11583682";
    value.security_patch = sdk == 35 ? "2024-11-05" : "2024-05-05";
    value.brand = "google";
    value.manufacturer = "Google";
    value.marketname = model;
    value.soc_manufacturer = "Google";
    value.soc_model = "Tensor G3";
    return value;
}

static void test_parse_and_roundtrip() {
    sbxpersona::Persona source = persona("Pixel 8 Pro", 34);
    std::string line = sbxpersona::serialize_line(source);
    sbxpersona::Persona parsed;
    std::string error;
    CHECK(sbxpersona::parse_line(line, parsed, error), "serialized persona parses");
    CHECK(parsed.model == source.model && parsed.sdk == 34,
          "roundtrip preserves model and SDK");
    CHECK(!sbxpersona::parse_line("too\tfew\tcolumns", parsed, error),
          "short persona row rejected");
    std::string invalid = line;
    size_t patch = invalid.find("2024-05-05");
    invalid.replace(patch, 10, "2024-13-05");
    CHECK(!sbxpersona::parse_line(invalid, parsed, error),
          "invalid security patch rejected");
    CHECK(sbxpersona::parse_candidate(line, 34, parsed, error),
          "one exact-SDK candidate accepted");
    CHECK(!sbxpersona::parse_candidate(line + line, 34, parsed, error),
          "multi-row candidate rejected");
    CHECK(!sbxpersona::parse_candidate(line, 35, parsed, error),
          "wrong-SDK candidate rejected");
}

static void test_source_priority() {
    const sbxpersona::Persona override_value = persona("Override", 35);
    const sbxpersona::Persona cache_value = persona("Cache", 35);
    const sbxpersona::Persona extension_value = persona("Extension", 35);
    const sbxpersona::Persona builtin_value = persona("Builtin", 35);
    sbxpersona::Selection selected;
    std::vector<std::string> warnings;
    std::string error;

    CHECK(sbxpersona::select(35, sbxpersona::serialize_line(override_value),
                             sbxpersona::serialize_line(cache_value),
                             {extension_value}, {builtin_value}, 0,
                             selected, warnings, error),
          "valid override selection succeeds");
    CHECK(selected.source == sbxpersona::Source::Override &&
              selected.persona.model == "Override",
          "override has first priority");

    CHECK(sbxpersona::select(
              35, sbxpersona::serialize_line(override_value) +
                      sbxpersona::serialize_line(cache_value),
              sbxpersona::serialize_line(cache_value), {extension_value},
              {builtin_value}, 0, selected, warnings, error) &&
              selected.source == sbxpersona::Source::Cache && !warnings.empty(),
          "multi-row override is rejected before cache fallback");
    CHECK(sbxpersona::select(
              35, "", sbxpersona::serialize_line(cache_value) +
                              sbxpersona::serialize_line(override_value),
              {extension_value}, {builtin_value}, 0, selected, warnings, error) &&
              selected.source == sbxpersona::Source::Extension && !warnings.empty(),
          "multi-row cache is rejected before reviewed pool fallback");

    CHECK(sbxpersona::select(35, sbxpersona::serialize_line(persona("Wrong", 34)),
                             sbxpersona::serialize_line(cache_value),
                             {extension_value}, {builtin_value}, 0,
                             selected, warnings, error),
          "wrong-SDK override falls back");
    CHECK(selected.source == sbxpersona::Source::Cache && !warnings.empty(),
          "cache follows invalid override with warning");

    for (uint64_t selector = 0; selector < 8; ++selector) {
        CHECK(sbxpersona::select(35, "", "", {extension_value},
                                 {builtin_value}, selector, selected, warnings,
                                 error) &&
                  selected.source == sbxpersona::Source::Extension &&
                  selected.persona.model == "Extension",
              "exact-SDK extensions strictly outrank builtins");
    }

    CHECK(sbxpersona::select(35, "", "", {persona("Old Extension", 34)},
                             {builtin_value}, 0, selected, warnings, error),
          "builtin fallback succeeds");
    CHECK(selected.source == sbxpersona::Source::Builtin,
          "builtin used when no exact extension exists");
    CHECK(!sbxpersona::select(36, "", "", {}, {builtin_value}, 0,
                              selected, warnings, error),
          "unsupported runtime SDK fails");
}

static void test_compiled_coverage() {
    const std::vector<sbxpersona::Persona> compiled =
        sbxpersona::compiled_personas();
    for (int sdk = 31; sdk <= 36; ++sdk) {
        bool found = false;
        for (const auto& value : compiled) {
            if (value.sdk == sdk) {
                found = true;
                break;
            }
        }
        CHECK(found, "compiled catalog covers supported runtime SDK");
    }
    bool verified_radio = false;
    for (const auto& value : compiled) {
        if (value.sdk == 34 && value.device == "cheetah" &&
            value.security_patch == "2023-11-01" &&
            value.radio == "g5300q-230626-230818-B-10679446") {
            verified_radio = true;
        }
    }
    CHECK(verified_radio, "SDK 34 catalog preserves reviewed Pixel 7 Pro metadata");
}

static std::map<std::string, std::string> valid_identity() {
    std::map<std::string, std::string> values = {
        {"BRAND", "google"}, {"MANUFACTURER", "Google"},
        {"MODEL", "Pixel 8 Pro"}, {"MARKETNAME", "Pixel 8 Pro"},
        {"DEVICE", "husky"}, {"PRODUCT", "husky"}, {"BOARD", "husky"},
        {"HARDWARE", "zuma"}, {"BOARD_PLATFORM", "zuma"},
        {"ID", "AP1A.240505.004"}, {"DISPLAY", "AP1A.240505.004"},
        {"INCREMENTAL", "11583682"}, {"RELEASE", "14"},
        {"SECURITY_PATCH", "2024-05-05"}, {"HOST", "abfarm-123"},
        {"USER", "android-build"}, {"TYPE", "user"},
        {"TAGS", "release-keys"}, {"FLAVOR", "husky-user"},
        {"SERIAL", "AABBCCDDEEFF0011"}, {"ANDROID_ID", "0123456789abcdef"},
        {"BUILD_TIME_UTC", "1714446000"},
        {"BUILD_DATE", sbxid::utc_date_string(1714446000ULL)},
        {"APPLOG_EPOCH", "1714446000000"},
    };
    values["FINGERPRINT"] = "google/husky/husky:14/AP1A.240505.004/11583682:user/release-keys";
    values["DESCRIPTION"] = "husky-user 14 AP1A.240505.004 11583682 release-keys";
    return values;
}

static bool validates(std::map<std::string, std::string> values) {
    sbxid::IdentitySnapshot snapshot;
    snapshot.values = std::move(values);
    sbxid::ValidationContext context;
    context.runtime_sdk = 34;
    std::string error;
    return sbxid::validate_snapshot(context, snapshot, error);
}

static void test_local_identity_validation() {
    auto values = valid_identity();
    values["GOOGLE_AID"] = "123e4567-e89b-42d3-a456-426614174000";
    values["BOOT_COUNT"] = "42";
    CHECK(validates(values), "valid remaining local identity fields accepted");

    auto bad_gaid = values;
    bad_gaid["GOOGLE_AID"] = "123e4567-e89b-12d3-a456-426614174000";
    CHECK(!validates(bad_gaid), "non-v4 GAID rejected");
    auto bad_boot = values;
    bad_boot["BOOT_COUNT"] = "1000001";
    CHECK(!validates(bad_boot), "oversized boot count rejected");
}

static void test_transaction_metadata() {
    sbxtxn::CanonicalMeta parsed;
    std::string error;
    const std::string run = "0123456789abcdef0123456789abcdef";
    const std::string identity = "BRAND=google\nMODEL=Pixel 8\n";
    const std::string identity_hash = sbxhash::sha256(identity);
    const std::string base_hash = sbxtxn::state_digest("", "");
    const std::string transaction =
        "version=1\nrun=" + run + "\nsource=cache\nidentity_sha256=" +
        identity_hash + "\nbase_state_sha256=" + base_hash + "\n";
    CHECK(sbxtxn::parse_pending_meta(transaction, parsed, error) &&
              parsed.run == run && parsed.source == "cache" &&
              sbxtxn::identity_matches(parsed, identity),
          "strict pending metadata binds canonical identity bytes");
    CHECK(!sbxtxn::identity_matches(parsed, identity + "\n"),
          "transaction metadata rejects changed identity bytes");
    CHECK(!sbxtxn::parse_transaction_meta(
              "version=1\nrun=" + run + "\nsource=cache\n", parsed, error),
          "hashless transaction metadata rejected");
    const std::string canonical =
        "version=1\nrun=" + run + "\nsource=cache\nidentity_sha256=" +
        identity_hash + "\n";
    CHECK(sbxtxn::parse_identity_meta(canonical, parsed, error) &&
              !sbxtxn::transaction_has_base(parsed),
          "canonical identity metadata omits pending-only base state");
    CHECK(!sbxtxn::parse_pending_meta(canonical, parsed, error),
          "pending metadata requires a base-state binding");
    CHECK(!sbxtxn::parse_identity_meta(transaction, parsed, error),
          "canonical identity metadata rejects a pending base-state binding");
    CHECK(!sbxtxn::parse_transaction_meta(
              transaction + "source=builtin\n", parsed, error),
          "duplicate transaction field rejected");
    const std::string legacy =
        "version=1\nsource=legacy\nidentity_sha256=" + identity_hash + "\n";
    CHECK(sbxtxn::parse_identity_meta(legacy, parsed, error) && parsed.legacy &&
              sbxtxn::identity_matches(parsed, identity),
          "exact hash-bound legacy migration metadata accepted");
    CHECK(!sbxtxn::parse_canonical_meta(
              legacy + "run=" + run + "\n", parsed, error),
          "ambiguous legacy metadata rejected");

    const std::string legacy_base_hash = sbxtxn::state_digest(identity, legacy);
    sbxtxn::CanonicalMeta pending_meta = parsed;
    pending_meta.legacy = false;
    pending_meta.run = run;
    pending_meta.source = "cache";
    pending_meta.identity_sha256 = identity_hash;
    pending_meta.base_state_sha256 = legacy_base_hash;
    const std::string pending = sbxtxn::serialize_meta(pending_meta);
    CHECK(sbxtxn::parse_pending_meta(pending, parsed, error) &&
              parsed.base_state_sha256 == legacy_base_hash,
          "pending metadata preserves its canonical base-state binding");
    CHECK(sbxtxn::state_digest(identity + "\n", legacy) != legacy_base_hash,
          "base-state digest changes when canonical identity bytes change");

    const std::string action_owner =
        "version=1\nkind=action\npid=123\nproc_start=456\nrun=" + run +
        "\ntoken=0123456789abcdef0123456789abcdef\n";
    sbxtxn::MutationOwner owner;
    CHECK(sbxtxn::parse_mutation_owner(action_owner, owner, error) &&
              owner.kind == "action" && owner.pid == 123 &&
              owner.proc_start == 456 && owner.run == run,
          "strict Action mutation owner accepted");
    const std::string standalone_owner =
        "version=1\nkind=standalone\npid=123\nproc_start=456\n"
        "token=0123456789abcdef0123456789abcdef\n";
    CHECK(sbxtxn::parse_mutation_owner(standalone_owner, owner, error) &&
              owner.kind == "standalone" && owner.run.empty(),
          "strict standalone mutation owner accepted");
    CHECK(!sbxtxn::parse_mutation_owner(
              standalone_owner + "run=" + run + "\n", owner, error),
          "standalone mutation owner rejects Action run field");
    std::string wrong_token = action_owner;
    wrong_token.replace(wrong_token.find("0123456789abcdef0123456789abcdef"),
                        32, "0123456789ABCDEF0123456789ABCDEF");
    CHECK(!sbxtxn::parse_mutation_owner(wrong_token, owner, error),
          "mutation owner rejects non-lowercase token");

    const std::string candidate = sbxpersona::serialize_line(persona("Cache", 35));
    const std::string provenance =
        "version=1\nschema=persona-v1\nparser=autopif-v1\n"
        "retrieved_utc=1720000000\nruntime_sdk=35\n"
        "candidate_sha256=" + sbxhash::sha256(candidate) +
        "\nadapter=pixel-ota-v1\n"
        "url=https://dl.google.com/developers/android/vic/images/ota/"
        "shiba_beta-ota-bp11.241210.004-a1bcf4f0.zip\n";
    CHECK(sbxtxn::validate_provenance(provenance, 35, candidate, error),
          "Pixel OTA provenance and canonical candidate hash accepted");
    CHECK(!sbxtxn::validate_provenance(provenance, 35, candidate + "\n", error),
          "provenance hash rejects changed candidate bytes");
    std::string unknown_adapter = provenance;
    const size_t adapter = unknown_adapter.find("adapter=pixel-ota-v1");
    unknown_adapter.replace(adapter, std::string("adapter=pixel-ota-v1").size(),
                            "adapter=unknown");
    CHECK(!sbxtxn::validate_provenance(unknown_adapter, 35, candidate, error),
          "unknown provenance adapter rejected");
    std::string wrong_sdk = provenance;
    const size_t sdk = wrong_sdk.find("runtime_sdk=35");
    wrong_sdk.replace(sdk, std::string("runtime_sdk=35").size(),
                      "runtime_sdk=34");
    CHECK(!sbxtxn::validate_provenance(wrong_sdk, 35, candidate, error),
          "wrong provenance runtime SDK rejected");
    std::string invalid_url = provenance;
    const size_t url = invalid_url.find("https://dl.google.com/");
    invalid_url.replace(url, std::string("https://").size(), "http://");
    CHECK(!sbxtxn::validate_provenance(invalid_url, 35, candidate, error),
          "non-HTTPS provenance URL rejected");
    std::string wrong_host = provenance;
    const size_t host = wrong_host.find("dl.google.com");
    wrong_host.replace(host, std::string("dl.google.com").size(),
                       "example.com");
    CHECK(!sbxtxn::validate_provenance(wrong_host, 35, candidate, error),
          "non-Google OTA provenance host rejected");
}

static void test_sha256() {
    CHECK(sbxhash::sha256("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "empty SHA-256 vector matches");
    CHECK(sbxhash::sha256("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "abc SHA-256 vector matches");
}

int main() {
    test_parse_and_roundtrip();
    test_source_priority();
    test_compiled_coverage();
    test_local_identity_validation();
    test_transaction_metadata();
    test_sha256();
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
