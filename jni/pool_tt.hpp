
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
    const char* soc;              // Build.SOC_MODEL (SOC_MANUFACTURER is always "Google" for Pixel)
    // Baseband prefix, up to but not including the first "-<date>-<date>-B-<incremental>".
    // Anti-fraud SDKs cross-check RADIO against SoC generation, so this must match
    // real Pixel modem strings (from Google factory image release notes):
    //   Tensor  (Pixel 6/6a)                  -> g5123b
    //   Tensor G2 (Pixel 7 series)            -> g5300b
    //   Tensor G3 (Pixel 8 series)            -> g5300q
    //   Tensor G4 (Pixel 9 series)            -> g5300q  (Samsung Exynos 5400)
    //   Tensor G5 (Pixel 10 series)           -> g5300s  (later Samsung modem revision)
    const char* baseband_prefix;
};

// Notes on (INCREMENTAL, security_patch) alignment:
//   * INCREMENTAL is Google's monotonically increasing build id. It advances ~30-60k
//     per month across the whole Pixel line. Setting a stale INCREMENTAL with a
//     freshly-dated SECURITY_PATCH is a detection vector (e.g. INCREMENTAL from
//     mid-2024 combined with a 2026-07 patch date is impossible on a real device).
//   * Values below are approximate 2026-mid to 2026-Q3 builds for each supported
//     Pixel. When rotating the pool refresh, keep INCREMENTAL and SECURITY_PATCH
//     drifting in lock-step.
static constexpr PixelEntry TT_POOL[] = {
    {"Pixel 6",        "oriole",  "oriole",  "oriole",  35, "15", "BP2A.250705.004", "13051260", "2026-06-05", "Tensor",    "g5123b"},
    {"Pixel 6a",       "bluejay", "bluejay", "bluejay", 35, "15", "BP2A.250705.004", "13051261", "2026-06-05", "Tensor",    "g5123b"},
    {"Pixel 7",        "panther", "panther", "panther", 35, "15", "BP2A.250705.008", "13051270", "2026-06-05", "Tensor G2", "g5300b"},
    {"Pixel 7 Pro",    "cheetah", "cheetah", "cheetah", 35, "15", "BP2A.250705.008", "13051271", "2026-06-05", "Tensor G2", "g5300b"},
    {"Pixel 7a",       "lynx",    "lynx",    "lynx",    35, "15", "BP2A.250705.008", "13051272", "2026-06-05", "Tensor G2", "g5300b"},
    {"Pixel 8",        "shiba",   "shiba",   "shiba",   36, "16", "BP2A.250705.015", "13051307", "2026-07-05", "Tensor G3", "g5300q"},
    {"Pixel 8 Pro",    "husky",   "husky",   "husky",   36, "16", "BP2A.250705.015", "13051308", "2026-07-05", "Tensor G3", "g5300q"},
    {"Pixel 8a",       "akita",   "akita",   "akita",   36, "16", "BP2A.250705.015", "13051309", "2026-07-05", "Tensor G3", "g5300q"},
    {"Pixel 9",        "tokay",   "tokay",   "tokay",   36, "16", "BP2A.250705.021", "13051340", "2026-07-05", "Tensor G4", "g5300q"},
    {"Pixel 9 Pro",    "caiman",  "caiman",  "caiman",  36, "16", "BP2A.250705.021", "13051341", "2026-07-05", "Tensor G4", "g5300q"},
    {"Pixel 9 Pro XL", "komodo",  "komodo",  "komodo",  36, "16", "BP2A.250705.021", "13051342", "2026-07-05", "Tensor G4", "g5300q"},
    {"Pixel 9a",       "tegu",    "tegu",    "tegu",    36, "16", "BP2A.250705.021", "13051343", "2026-07-05", "Tensor G4", "g5300q"},
    {"Pixel 10",       "frankel", "frankel", "frankel", 36, "16", "BP2A.250705.030", "13051401", "2026-07-05", "Tensor G5", "g5300s"},
    {"Pixel 10 Pro",   "blazer",  "blazer",  "blazer",  36, "16", "BP2A.250705.030", "13051402", "2026-07-05", "Tensor G5", "g5300s"},
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
