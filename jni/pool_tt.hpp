// ternak_tt v2.2 - pool_tt.hpp
//
// v2.2 changes:
//   - P2-G: `_all_have_brand` rewritten sebagai constexpr `for` loop
//     (C++14+) — recursive template instantiation depth turun dari O(N)
//     ke O(1). Compile lebih cepat dan tidak akan menyentuh limit -ftemplate-depth.
//   - No pool data changes.

#pragma once

#include <cstddef>

struct DeviceEntry {
    const char* model;
    const char* device;
    const char* product;
    const char* board;
    int         sdk;
    const char* release;
    const char* id;
    const char* incremental;
    const char* security_patch;
    const char* brand;         // "google" | "samsung" | "xiaomi" | "OnePlus"
    const char* manufacturer;  // "Google" | "Samsung" | "Xiaomi" | "OnePlus"
};

// Legacy alias so callers written against v2.0 continue to compile.
using PixelEntry = DeviceEntry;

static constexpr DeviceEntry TT_POOL[] = {
    // ---- Google Pixel (Tensor G1/G2) ----
    {"Pixel 6",     "oriole",   "oriole",         "oriole",   33, "13", "TQ3A.230901.001", "10750268",             "2026-01-05", "google",  "Google"},
    {"Pixel 6a",    "bluejay",  "bluejay",        "bluejay",  34, "14", "UP1A.231105.001", "11010452",             "2026-02-05", "google",  "Google"},
    {"Pixel 7",     "panther",  "panther",        "panther",  34, "14", "UP1A.231105.003", "11015216",             "2026-03-05", "google",  "Google"},
    {"Pixel 7 Pro", "cheetah",  "cheetah",        "cheetah",  34, "14", "UP1A.231105.003", "11015217",             "2026-03-05", "google",  "Google"},
    {"Pixel 7a",    "lynx",     "lynx",           "lynx",     34, "14", "UP1A.231105.003", "11015218",             "2026-04-05", "google",  "Google"},
    // ---- Google Pixel (Tensor G3) ----
    {"Pixel 8",     "shiba",    "shiba",          "shiba",    35, "15", "AP3A.240905.015", "12244875",             "2026-05-05", "google",  "Google"},
    {"Pixel 8 Pro", "husky",    "husky",          "husky",    35, "15", "AP3A.240905.015", "12244876",             "2026-06-05", "google",  "Google"},
    {"Pixel 8a",    "akita",    "akita",          "akita",    35, "15", "AP3A.240905.015", "12244877",             "2026-06-05", "google",  "Google"},
    // ---- Google Pixel (Tensor G4) ----
    {"Pixel 9",       "tokay",  "tokay",  "tokay",  35, "15", "AP3A.241005.015", "12580210", "2026-07-05", "google", "Google"},
    {"Pixel 9 Pro",   "caiman", "caiman", "caiman", 35, "15", "AP3A.241005.015", "12580211", "2026-07-05", "google", "Google"},
    {"Pixel 9 Pro XL","komodo", "komodo", "komodo", 35, "15", "AP3A.241005.015", "12580212", "2026-07-05", "google", "Google"},
    {"Pixel 9a",      "tegu",   "tegu",   "tegu",   35, "15", "AP3A.241005.015", "12580213", "2026-06-05", "google", "Google"},

    // ---- Samsung Galaxy S24 series ----
    {"SM-S928B", "e3q", "e3qxxx", "e3q", 34, "14", "UP1A.231005.007", "S928BXXU2AXCA", "2026-03-01", "samsung", "Samsung"},
    {"SM-S926B", "e2q", "e2qxxx", "e2q", 34, "14", "UP1A.231005.007", "S926BXXU2AXCA", "2026-03-01", "samsung", "Samsung"},
    {"SM-S921B", "e1q", "e1qxxx", "e1q", 34, "14", "UP1A.231005.007", "S921BXXU2AXCA", "2026-04-01", "samsung", "Samsung"},
    // ---- Samsung Galaxy A5x / A3x ----
    {"SM-A556E", "a55x", "a55xxx", "a55x", 34, "14", "UP1A.231005.007", "A556EXXU3BXCA", "2026-02-01", "samsung", "Samsung"},
    {"SM-A356E", "a35x", "a35xxx", "a35x", 34, "14", "UP1A.231005.007", "A356EXXU2BXCA", "2026-02-01", "samsung", "Samsung"},

    // ---- Xiaomi flagship ----
    {"2312DRA50G", "shennong", "shennong_global", "shennong", 34, "14", "UKQ1.230917.001", "V816.0.13.0.UNCMIXM", "2026-04-05", "xiaomi", "Xiaomi"},
    {"23049PCD8G", "aurora",   "aurora_global",   "aurora",   34, "14", "UKQ1.230804.001", "V816.0.10.0.UNBMIXM", "2026-05-05", "xiaomi", "Xiaomi"},

    // ---- OnePlus / Realme ----
    {"CPH2609", "OP5F0FL1", "OP5F0FL1EEA", "kalama", 34, "14", "UKQ1.230924.001", "V.C.30_1000", "2026-05-05", "OnePlus", "OnePlus"},
    {"CPH2451", "OP595DL1", "OP595DL1EEA", "kalama", 34, "14", "UKQ1.230924.001", "V.C.28_2000", "2026-06-05", "OnePlus", "OnePlus"},

    // ---- Google Pixel 10 (Tensor G5) ----
    {"Pixel 10",     "frankel", "frankel", "frankel", 36, "16", "BP1A.250705.006", "13051207", "2026-07-05", "google", "Google"},
    {"Pixel 10 Pro", "blazer",  "blazer",  "blazer",  36, "16", "BP1A.250705.006", "13051208", "2026-07-05", "google", "Google"},
};

static constexpr std::size_t TT_POOL_SIZE = sizeof(TT_POOL) / sizeof(TT_POOL[0]);

// Compile-time consistency check: every entry must have brand + manufacturer.
// v2.2: rewritten as a constexpr `for` loop (C++14+) — O(1) template depth.
namespace _tt_pool_check {
    template <std::size_t N>
    constexpr bool _all_have_brand(const DeviceEntry (&pool)[N]) {
        for (std::size_t i = 0; i < N; ++i) {
            if (!(pool[i].brand && pool[i].brand[0] &&
                  pool[i].manufacturer && pool[i].manufacturer[0])) {
                return false;
            }
        }
        return true;
    }
    static_assert(_all_have_brand(TT_POOL),
                  "TT_POOL entry missing brand or manufacturer");
}
