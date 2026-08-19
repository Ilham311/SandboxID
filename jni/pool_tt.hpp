#pragma once

struct PixelEntry {
    const char* model;
    const char* device;
    const char* product;
    const char* board;
    const char* platform;
    int         sdk;
    const char* release;
    const char* id;
    const char* incremental;
    const char* security_patch;
};

static constexpr PixelEntry TT_POOL[] = {
    {"Pixel 6",        "oriole",  "oriole",  "oriole",  "gs101",   33, "13", "TQ3A.230901.001", "10750268", "2026-06-05"},
    {"Pixel 6a",       "bluejay", "bluejay", "bluejay", "gs101",   34, "14", "UP1A.231105.001", "11010452", "2026-06-05"},
    {"Pixel 7",        "panther", "panther", "panther", "gs201",   34, "14", "UP1A.231105.003", "11015216", "2026-06-05"},
    {"Pixel 7 Pro",    "cheetah", "cheetah", "cheetah", "gs201",   34, "14", "UP1A.231105.003", "11015217", "2026-06-05"},
    {"Pixel 7a",       "lynx",    "lynx",    "lynx",    "gs201",   34, "14", "UP1A.231105.003", "11015218", "2026-06-05"},
    {"Pixel 8",        "shiba",   "shiba",   "shiba",   "zuma",    35, "15", "AP3A.240905.015", "12244875", "2026-07-05"},
    {"Pixel 8 Pro",    "husky",   "husky",   "husky",   "zuma",    35, "15", "AP3A.240905.015", "12244876", "2026-07-05"},
    {"Pixel 8a",       "akita",   "akita",   "akita",   "zuma",    35, "15", "AP3A.240905.015", "12244877", "2026-07-05"},
    {"Pixel 9",        "tokay",   "tokay",   "tokay",   "zumapro", 35, "15", "AP3A.241005.015", "12580210", "2026-07-05"},
    {"Pixel 9 Pro",    "caiman",  "caiman",  "caiman",  "zumapro", 35, "15", "AP3A.241005.015", "12580211", "2026-07-05"},
    {"Pixel 9 Pro XL", "komodo",  "komodo",  "komodo",  "zumapro", 35, "15", "AP3A.241005.015", "12580212", "2026-07-05"},
    {"Pixel 9a",       "tegu",    "tegu",    "tegu",    "zumapro", 35, "15", "AP3A.241005.015", "12580213", "2026-07-05"},
    {"Pixel 10",       "frankel", "frankel", "frankel", "laguna",  36, "16", "BP1A.250705.006", "13051207", "2026-07-05"},
    {"Pixel 10 Pro",   "blazer",  "blazer",  "blazer",  "laguna",  36, "16", "BP1A.250705.006", "13051208", "2026-07-05"},
};
