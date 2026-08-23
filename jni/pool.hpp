#pragma once

struct PixelEntry {
    const char* model;
    const char* device;
    const char* product;
    const char* board;
    const char* platform;
    const char* soc_model;
    int         sdk;
    const char* release;
    const char* id;
    const char* incremental;
    const char* security_patch;
};

static constexpr PixelEntry SBX_POOL[] = {
    {"Pixel 6",        "oriole",  "oriole",  "oriole",  "gs101",   "GS101", 33, "13", "TQ3A.230901.001", "10750268", "2023-09-05"},
    {"Pixel 6a",       "bluejay", "bluejay", "bluejay", "gs101",   "GS101", 34, "14", "UP1A.231105.001", "11010452", "2023-11-05"},
    {"Pixel 7",        "panther", "panther", "panther", "gs201",   "GS201", 34, "14", "UP1A.231105.003", "11015216", "2023-11-05"},
    {"Pixel 7 Pro",    "cheetah", "cheetah", "cheetah", "gs201",   "GS201", 34, "14", "UP1A.231105.003", "11015217", "2023-11-05"},
    {"Pixel 7a",       "lynx",    "lynx",    "lynx",    "gs201",   "GS201", 34, "14", "UP1A.231105.003", "11015218", "2023-11-05"},
    {"Pixel 8",        "shiba",   "shiba",   "shiba",   "zuma",    "GS301", 35, "15", "AP3A.240905.015", "12244875", "2024-09-05"},
    {"Pixel 8 Pro",    "husky",   "husky",   "husky",   "zuma",    "GS301", 35, "15", "AP3A.240905.015", "12244876", "2024-09-05"},
    {"Pixel 8a",       "akita",   "akita",   "akita",   "zuma",    "GS301", 35, "15", "AP3A.240905.015", "12244877", "2024-09-05"},
    {"Pixel 9",        "tokay",   "tokay",   "tokay",   "zumapro", "GS401", 35, "15", "AP3A.241005.015", "12580210", "2024-10-05"},
    {"Pixel 9 Pro",    "caiman",  "caiman",  "caiman",  "zumapro", "GS401", 35, "15", "AP3A.241005.015", "12580211", "2024-10-05"},
    {"Pixel 9 Pro XL", "komodo",  "komodo",  "komodo",  "zumapro", "GS401", 35, "15", "AP3A.241005.015", "12580212", "2024-10-05"},
    {"Pixel 9a",       "tegu",    "tegu",    "tegu",    "zumapro", "GS401", 35, "15", "AP3A.241005.015", "12580213", "2024-10-05"},
    {"Pixel 10",       "frankel", "frankel", "frankel", "laguna",  "GS501", 36, "16", "BP1A.250705.006", "13051207", "2025-07-05"},
    {"Pixel 10 Pro",   "blazer",  "blazer",  "blazer",  "laguna",  "GS501", 36, "16", "BP1A.250705.006", "13051208", "2025-07-05"},
};
