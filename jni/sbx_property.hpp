#pragma once

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace sbxprop {

enum class ApplyMode {
    kRequired,
    kExistingOnly,
};

inline ApplyMode apply_mode_for_property(std::string_view key) {
    return key == "ro.build.expect.baseband"
               ? ApplyMode::kExistingOnly
               : ApplyMode::kRequired;
}

inline bool should_apply(ApplyMode mode, bool property_exists) {
    return mode == ApplyMode::kRequired || property_exists;
}

enum class ValueAction {
    kPassThrough,
    kMapped,
    kHidden,
};

struct ValueDecision {
    ValueAction action = ValueAction::kPassThrough;
    std::string mapped;
};

inline ValueDecision decide_value(bool substitution_enabled, bool hidden,
                                  const std::string* mapped) {
    if (!substitution_enabled) return {};
    if (hidden) return {ValueAction::kHidden, {}};
    if (mapped) return {ValueAction::kMapped, *mapped};
    return {};
}

inline std::string_view selected_value(const ValueDecision& decision,
                                       const char* genuine) {
    if (decision.action == ValueAction::kMapped) return decision.mapped;
    if (decision.action == ValueAction::kHidden) return {};
    return genuine ? std::string_view(genuine) : std::string_view();
}

template <typename Callback>
inline bool complete_callback(Callback callback, void* cookie,
                              const char* name, const char* genuine,
                              uint32_t serial, const ValueDecision& decision) {
    if (!callback) return false;
    const std::string_view selected = selected_value(decision, genuine);
    const std::string stable(selected);
    callback(cookie, name, stable.c_str(), serial);
    return true;
}

template <typename Callback>
struct CallbackRelay {
    Callback callback = nullptr;
    void* cookie = nullptr;
    bool completed = false;

    bool complete(const char* name, const char* genuine, uint32_t serial,
                  const ValueDecision& decision) {
        if (completed || !callback) return false;
        completed = true;
        return complete_callback(callback, cookie, name, genuine, serial, decision);
    }
};

template <typename ReadCallback, typename PropertyInfo,
          typename CallerCallback, typename RelayCallback>
inline bool dispatch_callback_read(ReadCallback read_callback,
                                   const PropertyInfo* property_info,
                                   CallerCallback caller_callback,
                                   RelayCallback relay_callback,
                                   void* relay_cookie) {
    if (!read_callback || !property_info || !caller_callback || !relay_callback)
        return false;
    read_callback(property_info, relay_callback, relay_cookie);
    return true;
}

inline size_t legacy_copy_length(std::string_view value) {
    return value.size() < 91u ? value.size() : 91u;
}

inline bool stable_release_runtime(std::string_view preview_sdk,
                                   std::string_view codename) {
    return preview_sdk == "0" && codename == "REL";
}

inline bool release_alias_property(std::string_view key) {
    return key == "ro.build.version.release_or_codename" ||
           key == "ro.product.build.version.release_or_codename" ||
           key == "ro.system.build.version.release_or_codename" ||
           key == "ro.system_ext.build.version.release_or_codename" ||
           key == "ro.vendor.build.version.release_or_codename" ||
           key == "ro.odm.build.version.release_or_codename";
}

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
