#pragma once
//
// sbx_mountinfo.hpp — PURE selector for the opt-in root / mount-trace hider (F6).
// NO syscalls, NO Zygisk: it takes the text of /proc/<pid>/mountinfo and returns
// the list of mount points to detach, so the risky "which mounts are root traces?"
// decision is host-unit-testable (see tests/native_read_test.cpp). companion.cpp
// owns the actual setns()/umount2(MNT_DETACH) loop and feeds this the mountinfo it
// read inside the target's mount namespace.
//
// Technique credit: snake-4/Zygisk-Assistant (MIT) — the reverse-order detach of
// overlay/tmpfs mounts sourced from magisk/KSU/APatch worker dirs under /data/adb
// and /debug_ramdisk. We deliberately DIVERGE from it (documented in CREDITS.md):
// no unshare-strip / setresuid PLT hooks, and a deliberately NARROW target set,
// because an over-aggressive umount is itself detectable ("Umount Detected").
//
// The overriding safety rule: this must NEVER select one of OUR OWN persona
// overlays (the per-app build.prop binds in BIND_ENTRIES, or anything under
// MODDIR), nor a bare partition root or /data — tearing those down would break the
// spoof or the device. is_protected() enforces that and is checked before is_trace().
//
// Limitation: mountinfo octal-escapes space/tab/newline in paths (\040 etc). We do
// not un-escape; our prefix/substring matches target manager paths that contain no
// such characters, so this only means an exotic path is left mounted (fail-safe).

#include "config.hpp"

#include <string>
#include <vector>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace sbxmnt {

// Parsed subset of one mountinfo row (only the fields the selector needs).
struct MountRow {
    std::string mount_point;   // field 5 (0-based index 4)
    std::string fstype;        // first token after the " - " separator
    std::string source;        // second token after the separator
};

inline bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && std::memcmp(s.data(), p.data(), p.size()) == 0;
}

inline bool icontains(const std::string& hay, const char* needle) {
    std::string h = hay, ndl = needle;
    for (char& c : h)   c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : ndl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(ndl) != std::string::npos;
}

// Parse one mountinfo line. Returns false on a malformed/short line.
// Format: ID PARENT MAJ:MIN ROOT MOUNT_POINT OPTS [optional...] - FSTYPE SOURCE SUPEROPTS
inline bool parse_mountinfo_line(const std::string& line, MountRow& out) {
    std::vector<std::string> tok;
    size_t i = 0, n = line.size();
    while (i < n) {
        while (i < n && line[i] == ' ') ++i;
        if (i >= n) break;
        size_t j = i;
        while (j < n && line[j] != ' ') ++j;
        tok.emplace_back(line.substr(i, j - i));
        i = j;
    }
    if (tok.size() < 7) return false;
    // Locate the " - " separator (first bare "-" at index >= 6); fstype/source follow.
    size_t dash = std::string::npos;
    for (size_t k = 6; k < tok.size(); ++k) if (tok[k] == "-") { dash = k; break; }
    if (dash == std::string::npos || dash + 2 >= tok.size()) return false;
    out.mount_point = tok[4];
    out.fstype      = tok[dash + 1];
    out.source      = tok[dash + 2];
    return true;
}

// Mounts we must NEVER detach: our own persona overlays + module tree, and the
// bare partition / data roots. Checked first so it always wins over is_trace().
inline bool is_protected(const MountRow& r) {
    const std::string& mp = r.mount_point;
    if (mp == "/" || mp == "/data") return true;
    static const char* const roots[] = {
        "/system", "/vendor", "/product", "/system_ext", "/odm",
    };
    for (const char* R : roots) if (mp == R) return true;

    // Our module tree (MOUNTDIR lives under MODDIR, so this covers both) — as a
    // mount point or as the backing source of a bind.
    const std::string moddir(sandboxid::MODDIR);
    if (starts_with(mp, moddir))       return true;
    if (starts_with(r.source, moddir)) return true;

    // Our persona build.prop binds (+ settings_secure.xml) land exactly on these.
    for (const auto& be : sandboxid::BIND_ENTRIES)
        if (mp == be.dst) return true;
    return false;
}

// A root-manager trace worth detaching (only consulted when !is_protected).
inline bool is_trace(const MountRow& r) {
    const std::string& mp  = r.mount_point;
    const std::string& src = r.source;
    const std::string& fs  = r.fstype;

    // Manager / module working dirs that surface inside an app's mount namespace.
    if (mp == "/data/adb" || starts_with(mp, "/data/adb/")) return true;
    if (starts_with(mp, "/debug_ramdisk"))                  return true;

    // Overlay/tmpfs whose backing source names a known root manager.
    if (fs == "overlay" || fs == "tmpfs") {
        if (icontains(src, "magisk") || icontains(src, "worker") ||
            icontains(src, "workdir") || icontains(src, "apatch") ||
            icontains(src, "/ksu") || src == "KSU")
            return true;
    }
    // Any mount explicitly sourced from magisk (e.g. a magisk bind onto /system/xbin).
    if (icontains(src, "magisk")) return true;
    return false;
}

// Parse the whole mountinfo blob and return the mount points to umount, in REVERSE
// of file order (deepest / most-recently-mounted first, so children detach before
// parents). Duplicates are preserved on purpose: stacked mounts need one umount each.
inline std::vector<std::string> select_umount_targets(const std::string& mountinfo) {
    std::vector<std::string> out;
    size_t i = 0, n = mountinfo.size();
    while (i < n) {
        size_t eol = mountinfo.find('\n', i);
        size_t end = (eol == std::string::npos) ? n : eol;
        if (end > i) {
            MountRow r;
            if (parse_mountinfo_line(mountinfo.substr(i, end - i), r) &&
                !is_protected(r) && is_trace(r))
                out.push_back(r.mount_point);
        }
        if (eol == std::string::npos) break;
        i = eol + 1;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace sbxmnt
