#pragma once

#include <cerrno>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sbxpersona {

struct Persona {
    std::string model;
    std::string device;
    std::string product;
    std::string board;
    std::string platform;
    int sdk = 0;
    std::string release;
    std::string id;
    std::string incremental;
    std::string security_patch;
    std::string brand;
    std::string manufacturer;
    std::string marketname;
    std::string soc_manufacturer;
    std::string soc_model;
    std::string radio;
};

enum class Source {
    None,
    Override,
    Cache,
    Extension,
    Builtin,
};

inline const char* source_name(Source source) {
    switch (source) {
        case Source::Override: return "override";
        case Source::Cache: return "cache";
        case Source::Extension: return "extension";
        case Source::Builtin: return "builtin";
        default: return "none";
    }
}

struct Selection {
    Source source = Source::None;
    Persona persona;
};

inline std::vector<Persona> compiled_personas() {
    std::vector<Persona> values;
    auto add = [&](const char* model, const char* device, const char* platform,
                   int sdk, const char* release, const char* id,
                   const char* incremental, const char* patch,
                   const char* radio = nullptr) {
        Persona value;
        value.model = model;
        value.device = device;
        value.product = device;
        value.board = device;
        value.platform = platform;
        value.sdk = sdk;
        value.release = release;
        value.id = id;
        value.incremental = incremental;
        value.security_patch = patch;
        if (radio) value.radio = radio;
        values.push_back(std::move(value));
    };
    add("Pixel 6", "oriole", "gs101", 31, "12", "SD1A.210817.036",
        "7805805", "2021-10-05");
    add("Pixel 6 Pro", "raven", "gs101", 32, "12", "SQ3A.220705.003",
        "8671607", "2022-07-05", "g5123b-100840-220505-B-8544885");
    add("Pixel 6", "oriole", "gs101", 33, "13", "TQ3A.230901.001",
        "10750268", "2023-09-05");
    add("Pixel 7 Pro", "cheetah", "gs201", 34, "14", "UP1A.231105.003",
        "11010452", "2023-11-01", "g5300q-230626-230818-B-10679446");
    add("Pixel 8", "shiba", "zuma", 35, "15", "AP3A.240905.015",
        "12244875", "2024-09-05");
    add("Pixel 10", "frankel", "laguna", 36, "16", "BP1A.250705.006",
        "13051207", "2025-07-05");
    return values;
}

inline bool is_tensor_platform(std::string_view platform) {
    return platform == "gs101" || platform == "gs201" || platform == "zuma" ||
           platform == "zumapro" || platform == "laguna";
}

inline bool decimal_sdk(const std::string& value, int& out) {
    if (value.empty()) return false;
    for (char c : value) if (c < '0' || c > '9') return false;
    errno = 0;
    char* end = nullptr;
    long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || parsed < 1 || parsed > 1000)
        return false;
    out = static_cast<int>(parsed);
    return true;
}

inline bool safe_field(std::string_view value, bool allow_empty = false) {
    if (value.empty()) return allow_empty;
    if (value.size() > 128) return false;
    for (unsigned char c : value) {
        if (c < 0x20 || c == 0x7f || c == '=' || c == '\t' || c == '\r' || c == '\n')
            return false;
    }
    return value.front() != ' ' && value.back() != ' ';
}

inline bool valid_patch(std::string_view patch) {
    if (patch.size() != 10 || patch[4] != '-' || patch[7] != '-') return false;
    for (size_t i = 0; i < patch.size(); ++i)
        if (i != 4 && i != 7 && (patch[i] < '0' || patch[i] > '9')) return false;
    int year = std::atoi(std::string(patch.substr(0, 4)).c_str());
    int month = std::atoi(std::string(patch.substr(5, 2)).c_str());
    int day = std::atoi(std::string(patch.substr(8, 2)).c_str());
    return year >= 2008 && year <= 2100 && month >= 1 && month <= 12 &&
           day >= 1 && day <= 31;
}

inline bool parse_line(std::string_view raw, Persona& out, std::string& error) {
    out = {};
    error.clear();
    std::string line(raw);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    if (line.empty() || line.front() == '#') {
        error = "empty or comment persona row";
        return false;
    }
    std::vector<std::string> columns;
    size_t position = 0;
    while (position <= line.size()) {
        size_t end = line.find('\t', position);
        if (end == std::string::npos) end = line.size();
        columns.emplace_back(line.substr(position, end - position));
        if (end == line.size()) break;
        position = end + 1;
    }
    if (columns.size() < 10 || columns.size() > 16) {
        error = "persona row must contain 10 to 16 tab-separated fields";
        return false;
    }
    int sdk = 0;
    if (!decimal_sdk(columns[5], sdk)) {
        error = "persona SDK is invalid";
        return false;
    }
    for (size_t i = 0; i < columns.size(); ++i) {
        bool optional = i >= 10;
        if (!safe_field(columns[i], optional)) {
            error = "persona field " + std::to_string(i + 1) + " is invalid";
            return false;
        }
    }
    if (!valid_patch(columns[9])) {
        error = "persona security patch is invalid";
        return false;
    }
    out.model = columns[0];
    out.device = columns[1];
    out.product = columns[2];
    out.board = columns[3];
    out.platform = columns[4];
    out.sdk = sdk;
    out.release = columns[6];
    out.id = columns[7];
    out.incremental = columns[8];
    out.security_patch = columns[9];
    if (columns.size() > 10) out.brand = columns[10];
    if (columns.size() > 11) out.manufacturer = columns[11];
    if (columns.size() > 12) out.marketname = columns[12];
    if (columns.size() > 13) out.soc_manufacturer = columns[13];
    if (columns.size() > 14) out.soc_model = columns[14];
    if (columns.size() > 15) out.radio = columns[15];
    if (!is_tensor_platform(out.platform) &&
        (out.brand.empty() || out.manufacturer.empty() ||
         out.soc_manufacturer.empty() || out.soc_model.empty())) {
        error = "non-Tensor persona requires explicit brand, manufacturer, and SoC fields";
        out = {};
        return false;
    }
    return true;
}

inline std::string serialize_line(const Persona& persona) {
    std::string out;
    const std::string fields[] = {
        persona.model, persona.device, persona.product, persona.board,
        persona.platform, std::to_string(persona.sdk), persona.release,
        persona.id, persona.incremental, persona.security_patch, persona.brand,
        persona.manufacturer, persona.marketname, persona.soc_manufacturer,
        persona.soc_model, persona.radio,
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (i) out.push_back('\t');
        out.append(fields[i]);
    }
    out.push_back('\n');
    return out;
}

inline bool parse_candidate(std::string_view text, int runtime_sdk,
                            Persona& out, std::string& error) {
    out = {};
    if (text.empty() || text.size() > 4096) {
        error = "persona candidate is empty or oversized";
        return false;
    }
    size_t end = text.find('\n');
    std::string_view row = end == std::string_view::npos ? text : text.substr(0, end);
    if (end != std::string_view::npos) {
        std::string_view remainder = text.substr(end + 1);
        if (!remainder.empty()) {
            error = "persona candidate must contain exactly one row";
            return false;
        }
    }
    Persona parsed;
    if (!parse_line(row, parsed, error)) return false;
    if (parsed.sdk != runtime_sdk) {
        error = "persona SDK " + std::to_string(parsed.sdk) +
                " does not match runtime SDK " + std::to_string(runtime_sdk);
        return false;
    }
    out = std::move(parsed);
    return true;
}

inline std::vector<Persona> parse_extensions(std::string_view text,
                                             std::vector<std::string>* warnings = nullptr) {
    std::vector<Persona> out;
    std::set<std::string> seen;
    size_t position = 0;
    size_t line_number = 0;
    while (position <= text.size()) {
        size_t end = text.find('\n', position);
        if (end == std::string_view::npos) end = text.size();
        ++line_number;
        std::string_view row = text.substr(position, end - position);
        if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
        if (!row.empty() && row.front() != '#') {
            Persona parsed;
            std::string error;
            if (parse_line(row, parsed, error)) {
                std::string key = std::to_string(parsed.sdk) + "|" + parsed.device +
                                  "|" + parsed.id + "|" + parsed.incremental;
                if (seen.emplace(key).second) out.push_back(std::move(parsed));
            } else if (warnings) {
                warnings->push_back("line " + std::to_string(line_number) + ": " + error);
            }
        }
        if (end == text.size()) break;
        position = end + 1;
    }
    return out;
}

inline bool select(int runtime_sdk, std::string_view override_text,
                   std::string_view cache_text,
                   const std::vector<Persona>& extensions,
                   const std::vector<Persona>& builtins, uint64_t selector,
                   Selection& out, std::vector<std::string>& warnings,
                   std::string& error) {
    out = {};
    warnings.clear();
    error.clear();
    Persona exact;
    std::string source_error;
    if (!override_text.empty()) {
        if (parse_candidate(override_text, runtime_sdk, exact, source_error)) {
            out = {Source::Override, std::move(exact)};
            return true;
        }
        warnings.push_back("override skipped: " + source_error);
    }
    if (!cache_text.empty()) {
        if (parse_candidate(cache_text, runtime_sdk, exact, source_error)) {
            out = {Source::Cache, std::move(exact)};
            return true;
        }
        warnings.push_back("cache skipped: " + source_error);
    }
    std::vector<const Persona*> candidates;
    for (const Persona& persona : extensions)
        if (persona.sdk == runtime_sdk)
            candidates.push_back(&persona);
    Source selected_source = Source::Extension;
    if (candidates.empty()) {
        selected_source = Source::Builtin;
        for (const Persona& persona : builtins)
            if (persona.sdk == runtime_sdk)
                candidates.push_back(&persona);
    }
    if (candidates.empty()) {
        error = "no reviewed exact persona for runtime SDK " +
                std::to_string(runtime_sdk) + "; update SandboxID";
        return false;
    }
    out.source = selected_source;
    out.persona = *candidates[selector % candidates.size()];
    return true;
}

}  // namespace sbxpersona
