#pragma once

#include "sbx_identity.hpp"
#include "sbx_sha256.hpp"

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace sbxtxn {

using Fields = std::map<std::string, std::string>;

struct CanonicalMeta {
    bool legacy = false;
    std::string run;
    std::string source;
    std::string identity_sha256;
    std::string base_state_sha256;
};

inline bool parse_fields(const std::string& raw, Fields& values,
                         std::string& error) {
    static const std::set<std::string> no_empty_values = {
        "adapter", "base_state_sha256", "candidate_sha256", "identity_sha256",
        "kind", "parser", "pid", "proc_start", "retrieved_utc", "run",
        "runtime_sdk", "schema", "source", "token", "url", "version",
    };
    values.clear();
    if (raw.empty() || raw.size() > 16u * 1024u ||
        raw.find('\0') != std::string::npos) {
        error = "metadata is empty or oversized";
        return false;
    }
    std::istringstream input(raw);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            error = "empty metadata line";
            return false;
        }
        size_t equals = line.find('=');
        if (equals == std::string::npos || equals == 0) {
            error = "malformed metadata line";
            return false;
        }
        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        if (key.size() > 64 || value.size() > 2048 ||
            (value.empty() && no_empty_values.count(key) != 0)) {
            error = "invalid metadata field size";
            return false;
        }
        for (unsigned char c : key) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_')) {
                error = "invalid metadata key";
                return false;
            }
        }
        for (unsigned char c : value) {
            if (c < 0x20 || c == 0x7f) {
                error = "invalid metadata value";
                return false;
            }
        }
        if (!values.emplace(std::move(key), std::move(value)).second) {
            error = "duplicate metadata key";
            return false;
        }
    }
    return !values.empty();
}

inline bool valid_lower_hex(const std::string& value, size_t width) {
    if (value.size() != width) return false;
    for (char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline bool valid_run_id(const std::string& value) {
    return valid_lower_hex(value, 32);
}

inline bool valid_sha256(const std::string& value) {
    return valid_lower_hex(value, 64);
}

inline bool valid_source(const std::string& value) {
    return value == "override" || value == "cache" || value == "extension" ||
           value == "builtin";
}

inline bool parse_canonical_meta(const std::string& raw, CanonicalMeta& out,
                                 std::string& error) {
    out = {};
    Fields values;
    if (!parse_fields(raw, values, error)) return false;
    auto version = values.find("version");
    auto source = values.find("source");
    if (version == values.end() || version->second != "1" ||
        source == values.end()) {
        error = "metadata version or source is missing";
        return false;
    }
    auto identity_sha256 = values.find("identity_sha256");
    if (values.size() == 3 && source->second == "legacy" &&
        identity_sha256 != values.end() &&
        valid_sha256(identity_sha256->second)) {
        out.legacy = true;
        out.source = "legacy";
        out.identity_sha256 = identity_sha256->second;
        return true;
    }
    auto run = values.find("run");
    auto base_state_sha256 = values.find("base_state_sha256");
    if ((values.size() != 4 && values.size() != 5) || run == values.end() ||
        identity_sha256 == values.end() || !valid_run_id(run->second) ||
        !valid_source(source->second) ||
        !valid_sha256(identity_sha256->second) ||
        (values.size() == 5 &&
         (base_state_sha256 == values.end() ||
          !valid_sha256(base_state_sha256->second)))) {
        error = "invalid canonical transaction metadata";
        return false;
    }
    out.run = run->second;
    out.source = source->second;
    out.identity_sha256 = identity_sha256->second;
    if (base_state_sha256 != values.end())
        out.base_state_sha256 = base_state_sha256->second;
    return true;
}

inline bool parse_transaction_meta(const std::string& raw, CanonicalMeta& out,
                                   std::string& error) {
    if (!parse_canonical_meta(raw, out, error)) return false;
    if (out.legacy) {
        error = "legacy metadata is not a transaction";
        return false;
    }
    return true;
}

inline bool transaction_has_base(const CanonicalMeta& meta) {
    return valid_sha256(meta.base_state_sha256);
}

inline bool parse_identity_meta(const std::string& raw, CanonicalMeta& out,
                                std::string& error) {
    if (!parse_canonical_meta(raw, out, error)) return false;
    if (!out.legacy && transaction_has_base(out)) {
        error = "pending base-state binding is not valid canonical metadata";
        return false;
    }
    return true;
}

inline bool parse_pending_meta(const std::string& raw, CanonicalMeta& out,
                               std::string& error) {
    if (!parse_transaction_meta(raw, out, error)) return false;
    if (!transaction_has_base(out)) {
        error = "pending metadata is missing base-state binding";
        return false;
    }
    return true;
}

inline std::string serialize_meta(const CanonicalMeta& meta) {
    if (meta.legacy)
        return "version=1\nsource=legacy\nidentity_sha256=" +
               meta.identity_sha256 + "\n";
    std::string out = "version=1\nrun=" + meta.run + "\nsource=" + meta.source +
                      "\nidentity_sha256=" + meta.identity_sha256 + "\n";
    if (!meta.base_state_sha256.empty())
        out += "base_state_sha256=" + meta.base_state_sha256 + "\n";
    return out;
}

inline std::string state_digest(const std::string& identity,
                                const std::string& metadata) {
    return sbxhash::sha256(std::to_string(identity.size()) + ":" + identity +
                           std::to_string(metadata.size()) + ":" + metadata);
}

inline bool identity_matches(const CanonicalMeta& meta,
                             const std::string& identity) {
    return valid_sha256(meta.identity_sha256) &&
           meta.identity_sha256 == sbxhash::sha256(identity);
}

inline bool decimal_u64(const std::string& value, uint64_t& out) {
    return sbxid::decimal_u64(value, out);
}

struct MutationOwner {
    std::string kind;
    uint64_t pid = 0;
    uint64_t proc_start = 0;
    std::string run;
    std::string token;
};

inline bool valid_owner_token(const std::string& value) {
    return valid_lower_hex(value, 32);
}

inline bool parse_mutation_owner(const std::string& raw, MutationOwner& out,
                                 std::string& error) {
    out = {};
    Fields values;
    if (!parse_fields(raw, values, error)) return false;
    auto version = values.find("version");
    auto kind = values.find("kind");
    auto pid = values.find("pid");
    auto start = values.find("proc_start");
    auto token = values.find("token");
    auto run = values.find("run");
    const bool action = kind != values.end() && kind->second == "action";
    const bool standalone = kind != values.end() && kind->second == "standalone";
    if (version == values.end() || version->second != "1" ||
        (!action && !standalone) || pid == values.end() ||
        start == values.end() || token == values.end() ||
        values.size() != (action ? 6u : 5u) ||
        (action && (run == values.end() || !valid_run_id(run->second))) ||
        (!action && run != values.end()) || !valid_owner_token(token->second)) {
        error = "invalid mutation owner metadata";
        return false;
    }
    uint64_t parsed_pid = 0;
    uint64_t parsed_start = 0;
    if (!decimal_u64(pid->second, parsed_pid) || parsed_pid == 0 ||
        !decimal_u64(start->second, parsed_start) || parsed_start == 0) {
        error = "invalid mutation owner process binding";
        return false;
    }
    out.kind = kind->second;
    out.pid = parsed_pid;
    out.proc_start = parsed_start;
    out.run = action ? run->second : std::string();
    out.token = token->second;
    return true;
}

inline bool validate_provenance(const std::string& raw, int runtime_sdk,
                                const std::string& candidate,
                                std::string& error) {
    Fields meta;
    if (!parse_fields(raw, meta, error)) return false;
    static const char* const required[] = {
        "version", "schema", "parser", "retrieved_utc", "runtime_sdk",
        "candidate_sha256", "adapter", "url",
    };
    if (meta.size() != sizeof(required) / sizeof(required[0])) {
        error = "unexpected provenance field";
        return false;
    }
    for (const char* key : required) {
        if (meta.find(key) == meta.end()) {
            error = std::string("missing provenance field ") + key;
            return false;
        }
    }
    if (meta["version"] != "1" || meta["schema"] != "persona-v1" ||
        meta["parser"] != "autopif-v1" ||
        meta["runtime_sdk"] != std::to_string(runtime_sdk)) {
        error = "provenance version/schema/parser/runtime SDK mismatch";
        return false;
    }
    uint64_t retrieved = 0;
    if (!decimal_u64(meta["retrieved_utc"], retrieved) || retrieved == 0) {
        error = "invalid retrieval timestamp";
        return false;
    }
    const std::string& url = meta["url"];
    static constexpr char kOtaPrefix[] =
        "https://dl.google.com/developers/android/vic/images/ota/";
    if (url.rfind(kOtaPrefix, 0) != 0 ||
        url.find_first_of(" \t\r\n?#") != std::string::npos ||
        url.size() <= sizeof(kOtaPrefix) - 1 ||
        url.size() > 2048 || url.substr(url.size() - 4) != ".zip" ||
        url.find("_beta-ota-") == std::string::npos) {
        error = "invalid Pixel OTA provenance URL";
        return false;
    }
    if (meta["adapter"] != "pixel-ota-v1") {
        error = "unsupported provenance adapter";
        return false;
    }
    if (meta["candidate_sha256"] != sbxhash::sha256(candidate)) {
        error = "candidate_sha256 does not match candidate content";
        return false;
    }
    return true;
}

}  // namespace sbxtxn
