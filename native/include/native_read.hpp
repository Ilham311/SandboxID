#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>

namespace sbxnr {

// ---- Tiny helpers kept inline ----

inline char hex_lc(unsigned v) { return "0123456789abcdef"[v & 0xF]; }

inline bool ends_with(const char* s, size_t sl, const char* suffix) {
    size_t xl = std::strlen(suffix);
    return sl >= xl && std::memcmp(s + sl - xl, suffix, xl) == 0;
}

inline bool ends_with(const std::string& s, const char* suffix) {
    return ends_with(s.c_str(), s.size(), suffix);
}

inline bool ci_contains(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

inline bool starts_with(const std::string& s, const char* p) {
    size_t n = std::strlen(p);
    return s.size() >= n && std::memcmp(s.data(), p, n) == 0;
}

inline std::string selinux_enforce_content() { return std::string("1"); }

// ---- Enums / structs ----

enum CpuAction { CPU_NONE = 0, CPU_QUALCOMM = 1, CPU_MTK = 2, CPU_STRIP = 3 };

enum Kind {
    NONE = 0, BOOTID, MAC, VERSION, MEMINFO, CPUINFO, SELINUX_ENFORCE,
    APPLOG_XML,
    BD_RAW_DID,
    BD_RAW_IID,
    BD_RAW_OPENUDID,
    BD_RAW_CLIENTUDID,
    BD_RAW_CDID,
};

struct ApplogIds {
    std::string did, iid, ssid, cdid, clientudid, openudid;
};

// ---- Declarations (defined in native_read.cpp) ----

uint64_t fnv1a(const std::string& s);
uint64_t splitmix64(uint64_t& x);
void fill_bytes(uint64_t seed, uint8_t* out, size_t n);

std::string uuid_from_seed(uint64_t seed);
std::string mac_from_seed(uint64_t seed);
bool is_valid_mac(const std::string& m);
std::string hex_from_seed(uint64_t seed, size_t nbytes);

std::string snowflake_from_seed(uint64_t seed, uint64_t epoch_ms);
ApplogIds make_applog_ids(uint64_t seed, uint64_t epoch_ms);
bool patch_applog_xml(const std::string& real, const ApplogIds& ids, std::string& out);
std::string applog_xml_synth(const ApplogIds& ids);

int release_major(const std::string& release);
const char* kernel_base(const std::string& platform, int rmaj);
const char* clang_for(int rmaj);
std::string synth_proc_version(const std::string& release,
                                const std::string& incremental,
                                const std::string& platform,
                                const std::string& host,
                                uint64_t seed);

int pixel_ram_gb(const std::string& model);
uint64_t ram_gb_to_memtotal_kb(int gb);
uint64_t round_up_marketing_gb(uint64_t real_kb);
std::string patch_meminfo(const std::string& real, int target_gb);

int cpu_action_for(const std::string& soc_manuf, const std::string& soc_model,
                   std::string& repl_out);
bool patch_cpuinfo(const std::string& real, int action,
                   const std::string& repl, std::string& out);

Kind classify(const char* path);

bool is_emulator_prop(const char* name);
bool is_custom_rom_prop(const char* name);
bool should_hide_prop(const char* name);
bool is_native_unsafe_prop(const char* name);

}
