// ternak_tt v2.2 — tt_lookup.hpp
// P1-F: compile-time lookup tables (linear scan) untuk peta kecil (N <= 64).
//
// Refs:
//   - https://joelfilho.com/blog/2020/compile_time_lookup_tables_in_cpp/
//   - https://martin.ankerl.com/2022/08/27/hashmap-bench-01/
//   - https://en.cppreference.com/w/cpp/container/map
//
// std::map<std::string, std::string> membutuhkan heap allocation per node,
// pointer-chasing (log N cache miss), dan lookup butuh std::string temporary
// bila key hanya string literal / const char*. Untuk peta <= 64 entri yang
// hidup selama proses (mis. peta L2 prop_get), linear scan pada array
// contiguous string_view menang telak: nol allocation, branch predictor
// friendly, semua entri muat dalam beberapa cache line.
//
// Contoh:
//   struct KV { std::string_view k; std::string_view v; };
//   static constexpr KV MY_TABLE[] = {
//       {"ro.build.user", "USER"},
//       {"ro.build.host", "HOST"},
//   };
//   if (const auto* v = tt::lookup_kv(MY_TABLE, "ro.build.user")) { ... }

#pragma once

#include <cstddef>
#include <string_view>

namespace tt {

// Generic linear-scan lookup for a fixed-size array of {key,value} entries.
// Requires `T` to have a public member `k` (string_view) as the key.
// Returns pointer to the whole entry, or nullptr if not found.
template <typename T, std::size_t N>
[[nodiscard]] inline const T* lookup_entry(const T (&table)[N],
                                           std::string_view key) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        if (table[i].k == key) return &table[i];
    }
    return nullptr;
}

// Shortcut untuk pola KV: returns value (string_view) atau string_view{} kosong.
template <typename T, std::size_t N>
[[nodiscard]] inline std::string_view lookup_kv(const T (&table)[N],
                                                std::string_view key) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        if (table[i].k == key) return table[i].v;
    }
    return {};
}

// Convenience shape for KV rows.
struct StrKV {
    std::string_view k;
    std::string_view v;
};

}  // namespace tt
