
#pragma once

struct PixelEntry {
    const char* model;
    const char* device;
    const char* product;
    const char* board;
    int         sdk;
    const char* release;
    const char* id;
    const char* incremental;
    const char* security_patch;
    const char* soc;   // Build.SOC_MODEL (SOC_MANUFACTURER is always "Google" for Pixel)
};

static constexpr PixelEntry TT_POOL[] = {
    {"Pixel 6",        "oriole",  "oriole",  "oriole",  33, "13", "TQ3A.230901.001", "10750268", "2026-06-05", "Tensor"},
    {"Pixel 6a",       "bluejay", "bluejay", "bluejay", 34, "14", "UP1A.231105.001", "11010452", "2026-06-05", "Tensor"},
    {"Pixel 7",        "panther", "panther", "panther", 34, "14", "UP1A.231105.003", "11015216", "2026-06-05", "Tensor G2"},
    {"Pixel 7 Pro",    "cheetah", "cheetah", "cheetah", 34, "14", "UP1A.231105.003", "11015217", "2026-06-05", "Tensor G2"},
    {"Pixel 7a",       "lynx",    "lynx",    "lynx",    34, "14", "UP1A.231105.003", "11015218", "2026-06-05", "Tensor G2"},
    {"Pixel 8",        "shiba",   "shiba",   "shiba",   35, "15", "AP3A.240905.015", "12244875", "2026-07-05", "Tensor G3"},
    {"Pixel 8 Pro",    "husky",   "husky",   "husky",   35, "15", "AP3A.240905.015", "12244876", "2026-07-05", "Tensor G3"},
    {"Pixel 8a",       "akita",   "akita",   "akita",   35, "15", "AP3A.240905.015", "12244877", "2026-07-05", "Tensor G3"},
    {"Pixel 9",        "tokay",   "tokay",   "tokay",   35, "15", "AP3A.241005.015", "12580210", "2026-07-05", "Tensor G4"},
    {"Pixel 9 Pro",    "caiman",  "caiman",  "caiman",  35, "15", "AP3A.241005.015", "12580211", "2026-07-05", "Tensor G4"},
    {"Pixel 9 Pro XL", "komodo",  "komodo",  "komodo",  35, "15", "AP3A.241005.015", "12580212", "2026-07-05", "Tensor G4"},
    {"Pixel 9a",       "tegu",    "tegu",    "tegu",    35, "15", "AP3A.241005.015", "12580213", "2026-07-05", "Tensor G4"},
    {"Pixel 10",       "frankel", "frankel", "frankel", 36, "16", "BP1A.250705.006", "13051207", "2026-07-05", "Tensor G5"},
    {"Pixel 10 Pro",   "blazer",  "blazer",  "blazer",  36, "16", "BP1A.250705.006", "13051208", "2026-07-05", "Tensor G5"},
};

// Mobile carriers used to build a region-consistent SIM persona. The pool is
// Indonesian to stay consistent with the default Asia/Jakarta timezone and
// id-ID locale (Ternak TT targets the ID market: TikTok + Grab). Each entry is
// a real MCC+MNC so gsm.*operator.numeric stays plausible.
struct CarrierEntry {
    const char* name;     // gsm.operator.alpha / gsm.sim.operator.alpha
    const char* mccmnc;   // gsm.operator.numeric / gsm.sim.operator.numeric
    const char* iso;      // gsm.operator.iso-country / gsm.sim.operator.iso-country
};

static constexpr CarrierEntry TT_CARRIERS[] = {
    {"Telkomsel",       "51010", "id"},
    {"IND Indosat",     "51001", "id"},
    {"XL Axiata",       "51011", "id"},
    {"3",               "51089", "id"},
    {"SMARTFREN",       "51009", "id"},
    {"axis",            "51008", "id"},
};
