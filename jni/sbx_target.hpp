#pragma once

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace sbxtarget {

struct TargetSet {
    std::vector<std::string> processes;
    std::vector<std::string> packages;
};

inline std::string trim(std::string_view value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t' ||
                           value[begin] == '\r' || value[begin] == '\n'))
        ++begin;
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\r' || value[end - 1] == '\n'))
        --end;
    return std::string(value.substr(begin, end - begin));
}

inline bool ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline bool ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

inline bool valid_package(std::string_view package) {
    if (package.empty() || package.size() > 255 ||
        package.find('.') == std::string_view::npos)
        return false;
    size_t segment_start = 0;
    while (segment_start < package.size()) {
        size_t segment_end = package.find('.', segment_start);
        if (segment_end == std::string_view::npos) segment_end = package.size();
        if (segment_end == segment_start || !ascii_alpha(package[segment_start]))
            return false;
        for (size_t i = segment_start + 1; i < segment_end; ++i) {
            char c = package[i];
            if (!(ascii_alpha(c) || ascii_digit(c) || c == '_')) return false;
        }
        segment_start = segment_end + 1;
    }
    return true;
}

inline bool valid_process_suffix(std::string_view suffix) {
    if (suffix.empty() || suffix.size() > 128) return false;
    for (char c : suffix) {
        if (!(ascii_alpha(c) || ascii_digit(c) || c == '_' || c == '.' || c == '-'))
            return false;
    }
    return true;
}

inline bool split_process(std::string_view process, std::string& package) {
    size_t colon = process.find(':');
    if (colon != std::string_view::npos &&
        process.find(':', colon + 1) != std::string_view::npos)
        return false;
    std::string_view base = colon == std::string_view::npos
                                ? process : process.substr(0, colon);
    if (!valid_package(base)) return false;
    if (colon != std::string_view::npos &&
        !valid_process_suffix(process.substr(colon + 1)))
        return false;
    package.assign(base);
    return true;
}

inline bool parse(std::string_view text, TargetSet& out, std::string& error) {
    out = {};
    error.clear();
    std::set<std::string> seen_processes;
    std::set<std::string> seen_packages;
    size_t position = 0;
    size_t line_number = 0;
    while (position <= text.size()) {
        size_t end = text.find('\n', position);
        if (end == std::string_view::npos) end = text.size();
        ++line_number;
        std::string line = trim(text.substr(position, end - position));
        size_t comment = line.find('#');
        if (comment != std::string::npos) line = trim(line.substr(0, comment));
        if (!line.empty()) {
            std::string package;
            if (!split_process(line, package)) {
                error = "invalid target on line " + std::to_string(line_number) +
                        ": " + line;
                out = {};
                return false;
            }
            if (seen_processes.emplace(line).second) out.processes.push_back(line);
            if (seen_packages.emplace(package).second) out.packages.push_back(package);
        }
        if (end == text.size()) break;
        position = end + 1;
    }
    return true;
}

}  // namespace sbxtarget
