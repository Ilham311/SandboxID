#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace sbxid {

inline constexpr size_t kDefaultMaxBlob = 64u * 1024u;
inline constexpr size_t kMaxLine = 4096u;
inline constexpr size_t kMaxKey = 64u;
inline constexpr size_t kMaxPropertyValue = 91u;
inline constexpr size_t kMaxIdentityValue = 1024u;

inline constexpr std::string_view kOperationalFlags[] = {
    "SBX_NATIVE_READ", "SBX_HIDE", "SBX_CPU_REVISION",
    "SBX_PROC_VERSION", "SBX_MEMINFO", "SBX_SYSFS_MAC",
};

inline bool operational_flag_key(std::string_view key) {
    for (std::string_view candidate : kOperationalFlags)
        if (key == candidate) return true;
    return false;
}

inline void preserve_operational_flags(
        const std::map<std::string, std::string>& current,
        std::map<std::string, std::string>& next) {
    for (std::string_view key : kOperationalFlags) {
        auto it = current.find(std::string(key));
        if (it != current.end()) next[it->first] = it->second;
    }
}

inline bool presentation_property_key(std::string_view key) {
    static constexpr std::string_view keys[] = {
        "BRAND", "MANUFACTURER", "MODEL", "MARKETNAME", "DEVICE", "PRODUCT",
        "BOARD", "HARDWARE", "BOARD_PLATFORM", "SOC_MANUFACTURER", "SOC_MODEL",
        "FINGERPRINT", "ID", "DISPLAY", "DESCRIPTION", "BOOTLOADER", "HOST",
        "USER", "TYPE", "TAGS", "INCREMENTAL", "RELEASE", "SECURITY_PATCH",
        "SERIAL", "RADIO", "FLAVOR", "BUILD_TIME_UTC", "BUILD_DATE", "SKU",
        "ODM_SKU", "GSM_OPERATOR_NUMERIC", "GSM_OPERATOR_ALPHA",
        "GSM_OPERATOR_ISO", "GSM_SIM_STATE",
    };
    for (std::string_view candidate : keys)
        if (key == candidate) return true;
    return false;
}

inline bool legacy_capability_key(std::string_view key) {
    static constexpr std::string_view keys[] = {
        "SUPPORTED_ABIS", "SUPPORTED_64_BIT_ABIS", "SUPPORTED_32_BIT_ABIS",
        "CPU_ABI", "CPU_ABI2", "BASE_OS", "MEDIA_PERFORMANCE_CLASS",
        "PREVIEW_SDK_INT", "PREVIEW_SDK_FINGERPRINT", "FIRST_API_LEVEL",
        "SYS_BOOT_COMPLETED", "BUILD_CHARACTERISTICS", "PERSIST_TIMEZONE",
        "DALVIK_HEAPGROWTHLIMIT", "MEDIACODEC_MIN_RATE", "MEDIACODEC_MAX_RATE",
        "DEBUG_FORCE_RTL", "MULTISIM_CONFIG", "VBMETA_DIGEST",
    };
    for (std::string_view candidate : keys)
        if (key == candidate) return true;
    return false;
}

inline bool forbidden_capability_key(std::string_view key) {
    return legacy_capability_key(key);
}

struct ValidationContext {
    int runtime_sdk = 0;
    size_t max_blob = kDefaultMaxBlob;
    bool drop_legacy_capabilities = false;
};

struct IdentitySnapshot {
    std::map<std::string, std::string> values;
    std::set<std::string> dropped_legacy_capabilities;
};

inline bool reject(std::string& error, const std::string& message) {
    error = message;
    return false;
}

inline bool valid_key(std::string_view key) {
    if (key.empty() || key.size() > kMaxKey || key[0] < 'A' || key[0] > 'Z')
        return false;
    for (char c : key)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

inline bool decimal_u64(const std::string& value, uint64_t& out) {
    if (value.empty()) return false;
    uint64_t parsed = 0;
    for (char c : value) {
        if (c < '0' || c > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (parsed > (UINT64_MAX - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    out = parsed;
    return true;
}

inline bool valid_date(const std::string& value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (size_t i = 0; i < value.size(); ++i)
        if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9')) return false;
    int y = std::atoi(value.substr(0, 4).c_str());
    int m = std::atoi(value.substr(5, 2).c_str());
    int d = std::atoi(value.substr(8, 2).c_str());
    if (y < 2008 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    struct tm tmv{};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m - 1;
    tmv.tm_mday = d;
    time_t t = timegm(&tmv);
    if (t == static_cast<time_t>(-1)) return false;
    struct tm check{};
    return gmtime_r(&t, &check) && check.tm_year == tmv.tm_year &&
           check.tm_mon == tmv.tm_mon && check.tm_mday == tmv.tm_mday;
}

inline bool valid_uuid(const std::string& value) {
    static constexpr std::string_view zero =
        "00000000-0000-0000-0000-000000000000";
    if (value == zero) return true;
    if (value.size() != 36) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') ||
                     (value[i] >= 'a' && value[i] <= 'f') ||
                     (value[i] >= 'A' && value[i] <= 'F'))) {
            return false;
        }
    }
    return (value[14] == '4') &&
           (value[19] == '8' || value[19] == '9' ||
            value[19] == 'a' || value[19] == 'b' ||
            value[19] == 'A' || value[19] == 'B');
}

inline bool valid_local_mac(const std::string& value) {
    if (value.size() != 17) return false;
    unsigned first_octet = 0;
    bool any_nonzero = false;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i % 3 == 2) {
            if (value[i] != ':') return false;
            continue;
        }
        char c = value[i];
        unsigned nibble = 0;
        if (c >= '0' && c <= '9') nibble = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') nibble = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') nibble = static_cast<unsigned>(c - 'A' + 10);
        else return false;
        if (nibble != 0) any_nonzero = true;
        if (i == 0) first_octet = nibble << 4;
        else if (i == 1) first_octet |= nibble;
    }
    return any_nonzero && (first_octet & 0x01u) == 0 && (first_octet & 0x02u) != 0;
}

inline bool valid_device_name(const std::string& value) {
    if (value.empty() || value.size() > 64 || value.front() == ' ' ||
        value.back() == ' ')
        return false;
    for (unsigned char c : value)
        if (c < 0x20 || c > 0x7e || c == '<' || c == '>' || c == '&' ||
            c == '\'' || c == '"')
            return false;
    return true;
}

inline std::string utc_date_string(uint64_t seconds) {
    time_t t = static_cast<time_t>(seconds);
    struct tm tmv{};
    if (!gmtime_r(&t, &tmv)) return {};
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S UTC %Y", &tmv) == 0)
        return {};
    return buf;
}

inline bool validate_snapshot(const ValidationContext& ctx,
                              const IdentitySnapshot& snapshot,
                              std::string& error) {
    static const char* const required[] = {
        "BRAND", "MANUFACTURER", "MODEL", "MARKETNAME", "DEVICE", "PRODUCT",
        "BOARD", "HARDWARE", "BOARD_PLATFORM", "FINGERPRINT", "ID", "DISPLAY",
        "DESCRIPTION", "INCREMENTAL", "RELEASE", "SECURITY_PATCH", "HOST", "USER",
        "TYPE", "TAGS", "FLAVOR", "SERIAL", "ANDROID_ID", "BUILD_TIME_UTC",
        "BUILD_DATE", "APPLOG_EPOCH",
    };
    for (const char* key : required) {
        auto it = snapshot.values.find(key);
        if (it == snapshot.values.end() || it->second.empty())
            return reject(error, std::string("missing required key ") + key);
    }
    auto get = [&](const char* key) -> const std::string& {
        return snapshot.values.find(key)->second;
    };

    const std::string fingerprint = get("BRAND") + "/" + get("PRODUCT") + "/" +
        get("DEVICE") + ":" + get("RELEASE") + "/" + get("ID") + "/" +
        get("INCREMENTAL") + ":" + get("TYPE") + "/" + get("TAGS");
    if (get("FINGERPRINT") != fingerprint)
        return reject(error, "FINGERPRINT contradicts canonical build fields");
    const std::string description = get("PRODUCT") + "-" + get("TYPE") + " " +
        get("RELEASE") + " " + get("ID") + " " + get("INCREMENTAL") + " " + get("TAGS");
    if (get("DESCRIPTION") != description)
        return reject(error, "DESCRIPTION contradicts canonical build fields");
    if (get("DISPLAY") != get("ID")) return reject(error, "DISPLAY must equal ID");
    if (get("FLAVOR") != get("PRODUCT") + "-" + get("TYPE"))
        return reject(error, "FLAVOR contradicts PRODUCT/TYPE");
    if (!valid_date(get("SECURITY_PATCH"))) return reject(error, "invalid SECURITY_PATCH");

    uint64_t build_time = 0;
    if (!decimal_u64(get("BUILD_TIME_UTC"), build_time) || build_time < 1199145600ULL ||
        build_time > 4133980800ULL)
        return reject(error, "invalid BUILD_TIME_UTC");
    if (utc_date_string(build_time) != get("BUILD_DATE"))
        return reject(error, "BUILD_DATE contradicts BUILD_TIME_UTC");
    uint64_t epoch = 0;
    if (!decimal_u64(get("APPLOG_EPOCH"), epoch) || epoch < 1199145600000ULL ||
        epoch > 4133980800000ULL)
        return reject(error, "invalid APPLOG_EPOCH");

    const std::string& aid = get("ANDROID_ID");
    if (aid.size() != 16) return reject(error, "ANDROID_ID must be 16 hex characters");
    for (char c : aid)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return reject(error, "ANDROID_ID must be hexadecimal");
    auto gaid = snapshot.values.find("GOOGLE_AID");
    if (gaid != snapshot.values.end() && !gaid->second.empty() && !valid_uuid(gaid->second))
        return reject(error, "invalid GOOGLE_AID");
    for (const char* key : {"WIFI_MAC", "BLUETOOTH_ADDR"}) {
        auto mac = snapshot.values.find(key);
        if (mac != snapshot.values.end() && !mac->second.empty() &&
            !valid_local_mac(mac->second))
            return reject(error, std::string("invalid locally administered ") + key);
    }
    auto bluetooth_name = snapshot.values.find("BLUETOOTH_NAME");
    if (bluetooth_name != snapshot.values.end() &&
        !valid_device_name(bluetooth_name->second))
        return reject(error, "invalid BLUETOOTH_NAME");
    auto boot_count = snapshot.values.find("BOOT_COUNT");
    if (boot_count != snapshot.values.end()) {
        uint64_t parsed = 0;
        if (!decimal_u64(boot_count->second, parsed) || parsed > 1000000ULL)
            return reject(error, "invalid BOOT_COUNT");
    }

    auto sdk = snapshot.values.find("SDK_INT");
    if (sdk != snapshot.values.end() && !sdk->second.empty()) {
        uint64_t parsed = 0;
        if (!decimal_u64(sdk->second, parsed) || parsed == 0 || parsed > 1000)
            return reject(error, "invalid SDK_INT metadata");
        if (ctx.runtime_sdk > 0 && parsed != static_cast<uint64_t>(ctx.runtime_sdk))
            return reject(error, "SDK_INT metadata differs from runtime SDK");
    }
    auto soc_manufacturer = snapshot.values.find("SOC_MANUFACTURER");
    auto soc_model = snapshot.values.find("SOC_MODEL");
    bool have_soc_manufacturer = soc_manufacturer != snapshot.values.end() &&
                                 !soc_manufacturer->second.empty();
    bool have_soc_model = soc_model != snapshot.values.end() && !soc_model->second.empty();
    if (have_soc_manufacturer != have_soc_model)
        return reject(error, "SOC_MANUFACTURER and SOC_MODEL must appear together");
    error.clear();
    return true;
}

inline bool serialize_identity_values(
        const std::map<std::string, std::string>& values,
        const std::string_view* order, size_t order_count,
        std::string& out) {
    out.clear();
    std::set<std::string_view> emitted;
    auto append = [&](const std::string& key, const std::string& value) -> bool {
        if (!valid_key(key) || legacy_capability_key(key) ||
            value.size() > kMaxIdentityValue)
            return false;
        if (value.find('\n') != std::string::npos ||
            value.find('\r') != std::string::npos ||
            value.find('\0') != std::string::npos)
            return false;
        out.append(key);
        out.push_back('=');
        out.append(value);
        out.push_back('\n');
        emitted.emplace(key);
        return out.size() <= kDefaultMaxBlob;
    };

    for (size_t i = 0; i < order_count; ++i) {
        auto it = values.find(std::string(order[i]));
        if (it != values.end() && !append(it->first, it->second)) {
            out.clear();
            return false;
        }
    }
    for (const auto& entry : values) {
        if (emitted.find(entry.first) != emitted.end()) continue;
        if (!append(entry.first, entry.second)) {
            out.clear();
            return false;
        }
    }
    return !out.empty();
}

inline bool parse_and_validate_identity(std::string_view blob,
                                        const ValidationContext& ctx,
                                        IdentitySnapshot& out,
                                        std::string& error) {
    out.values.clear();
    out.dropped_legacy_capabilities.clear();
    if (blob.empty()) return reject(error, "identity blob is empty");
    if (blob.size() > ctx.max_blob) return reject(error, "identity blob is oversized");
    if (blob.find('\0') != std::string_view::npos)
        return reject(error, "identity blob contains NUL");

    size_t pos = 0;
    size_t line_no = 0;
    while (pos <= blob.size()) {
        size_t end = blob.find('\n', pos);
        if (end == std::string_view::npos) end = blob.size();
        ++line_no;
        std::string_view line = blob.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() > kMaxLine)
            return reject(error, "identity line " + std::to_string(line_no) + " is oversized");
        for (unsigned char c : line)
            if (c < 0x20 && c != '\t')
                return reject(error, "identity line " + std::to_string(line_no) + " contains control bytes");

        if (!line.empty() && line.front() != '#') {
            size_t eq = line.find('=');
            if (eq == std::string_view::npos || eq == 0)
                return reject(error, "malformed identity line " + std::to_string(line_no));
            std::string_view key_view = line.substr(0, eq);
            std::string_view value_view = line.substr(eq + 1);
            if (!valid_key(key_view))
                return reject(error, "invalid key on identity line " + std::to_string(line_no));
            if (legacy_capability_key(key_view)) {
                if (!ctx.drop_legacy_capabilities)
                    return reject(error, "runtime capability key is not allowed: " +
                                         std::string(key_view));
                if (!out.dropped_legacy_capabilities.emplace(key_view).second)
                    return reject(error, "duplicate identity key " +
                                         std::string(key_view));
                if (end == blob.size()) break;
                pos = end + 1;
                continue;
            }
            if (value_view.size() > kMaxIdentityValue)
                return reject(error, "identity value is oversized on line " +
                                     std::to_string(line_no));
            if (presentation_property_key(key_view) &&
                value_view.size() > kMaxPropertyValue)
                return reject(error, "property value is oversized for key " +
                                     std::string(key_view));
            if ((!value_view.empty() && (value_view.front() == ' ' || value_view.front() == '\t' ||
                                         value_view.back() == ' ' || value_view.back() == '\t')))
                return reject(error, "surrounding whitespace on identity line " + std::to_string(line_no));
            std::string key(key_view);
            std::string value(value_view);
            if (!out.values.emplace(key, value).second)
                return reject(error, "duplicate identity key " + key);
        }
        if (end == blob.size()) break;
        pos = end + 1;
    }

    auto flag = [&](std::string_view key) -> bool {
        auto it = out.values.find(std::string(key));
        return it == out.values.end() || it->second == "0" || it->second == "1";
    };
    for (std::string_view key : kOperationalFlags)
        if (!flag(key))
            return reject(error, "invalid operational boolean flag");
    auto uptime = out.values.find("UPTIME_SECONDS");
    uint64_t parsed = 0;
    if (uptime != out.values.end() && !decimal_u64(uptime->second, parsed))
        return reject(error, "invalid UPTIME_SECONDS");
    auto uptime_human = out.values.find("UPTIME_HUMAN");
    if (uptime_human != out.values.end()) {
        if (uptime_human->second.empty() || uptime_human->second.size() > 64)
            return reject(error, "invalid UPTIME_HUMAN");
        for (unsigned char c : uptime_human->second)
            if (c < 0x20 || c > 0x7e)
                return reject(error, "invalid UPTIME_HUMAN");
    }
    return validate_snapshot(ctx, out, error);
}

}  // namespace sbxid
