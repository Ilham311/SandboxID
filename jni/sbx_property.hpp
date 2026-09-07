#pragma once

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace sbxprop {

class HandleNames {
public:
    bool remember(int64_t handle, const std::string& name) {
        if (handle == 0 || name.empty()) return false;
        std::lock_guard<std::mutex> lock(mu_);
        names_[handle] = name;
        return true;
    }

    bool find(int64_t handle, std::string& name) const {
        if (handle == 0) return false;
        std::lock_guard<std::mutex> lock(mu_);
        auto it = names_.find(handle);
        if (it == names_.end()) return false;
        name = it->second;
        return true;
    }

private:
    mutable std::mutex mu_;
    std::map<int64_t, std::string> names_;
};

inline bool parse_int64(const std::string& value, int64_t min_value,
                        int64_t max_value, int64_t& out) {
    const char* begin = value.c_str();
    while (*begin && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
    if (!*begin) return false;

    int base = begin[0] == '0' && (begin[1] == 'x' || begin[1] == 'X') ? 16 : 10;
    errno = 0;
    char* end = nullptr;
    long long parsed = std::strtoll(begin, &end, base);
    if (end == begin || !end || *end != '\0' || errno == ERANGE ||
        parsed < min_value || parsed > max_value)
        return false;
    out = static_cast<int64_t>(parsed);
    return true;
}

inline bool parse_bool(const std::string& value, bool& out) {
    if (value == "1" || value == "y" || value == "yes" ||
        value == "on" || value == "true") {
        out = true;
        return true;
    }
    if (value == "0" || value == "n" || value == "no" ||
        value == "off" || value == "false") {
        out = false;
        return true;
    }
    return false;
}

}  // namespace sbxprop
