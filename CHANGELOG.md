# Ternak TT — Changelog

Heading convention: use `### vX.Y.Z` (exact line match) so the CI's awk in
`.github/workflows/build.yml` picks up the section for `release_notes.md`.

### v1.1.2

**Low-priority hardening pass L1–L8 (2026-07-31).**

Code-review follow-up. Eight low-severity items from the professional review.
All are style / correctness / observability tweaks with no behavior change for
valid inputs.

- **L1 — local `map` shadowing `std::map`** — already fixed in v1.1.0 (tables
  live in `prop_map.hpp` namespace `ternak_tt`); no code change, verified.
- **L2 — include hygiene** — dropped six unused headers from `main.cpp`:
  `<sys/mount.h>`, `<sys/wait.h>`, `<memory>`, `<thread>`, `<chrono>`,
  `<fstream>`. Verified via grep that no symbol from these headers is
  referenced. Reduces preprocessor input by ~30k lines.
- **L3 — function-static maps promoted to namespace scope** — `tt_bool_spoof`,
  `tt_int_spoof`, `tt_long_spoof` (used only when `TT_DEBUG` is defined) now
  live as `inline const std::map` in `namespace tt_leak_tables`. Removes the
  per-call guard-variable check the C++11 thread-safe-static ABI mandates.
  Ref: cppstories.com 2025 "How to Avoid Thread-Safety Cost for Functions'
  static Variables".
- **L4 — opt-in strict/whitelist prop_get mode** — added compile-time flag
  `TT_STRICT_PROPS` (via CMake `option(TERNAK_TT_STRICT_PROPS ...)`). When
  ON, `hook_prop_get` returns `j_def` for any key not in the explicit
  whitelist instead of falling back to `__system_property_get`. Default OFF
  — existing permissive behavior unchanged.
- **L5 — `defaults` map in `val()`** — already fixed in v1.1.0 (now
  `identity_fallback_defaults()` in `prop_map.hpp` namespace); no code
  change, verified.
- **L6 — TelephonyManager null-return convention (REVISED after research)** —
  kept `nullptr` return; added documentation. Rationale from AOSP
  `frameworks/base` / `TelephonyManager.java`: the real method itself
  returns `null` when the telephony subsystem is unavailable, so apps doing
  the canonical `if (imei != null && !imei.isEmpty())` treat null as
  "unknown" — which is exactly the signal we want. Returning `""` would be
  interpreted as "IMEI is present but happens to be empty" (nonsense: real
  IMEI is always 14-15 digits) and could trigger fingerprint mismatches.
  Apps that NPE on null are buggy on real devices too (no-SIM, tablet).
- **L7 — dead `CMD_CHECK_TT`** — removed from both `main.cpp` and
  `companion.cpp` enum. Never handled in any switch. The two surviving
  commands renumbered in lockstep: `CMD_GET_IDENTITY = 1`,
  `CMD_DO_MOUNTS = 2`.
- **L8 — release logcat visibility** — `LOGI` now compiles out in release
  (same as `LOGD`). Only `LOGE` (hard errors from crash watchdog + companion
  failures) still reaches logcat. `LOG_TAG` shortened from "TernakTT" to
  "TT" in release so a spying app can't `grep -i ternak` logcat to detect
  the module. In debug builds nothing changes. Reference pattern: VPN Hide
  v0.7.0 and Zygisk-Assistant both default to near-silent logcat.

**Verification:** `main.cpp` braces 180/180, parens 632/632, 814 lines.
`companion.cpp` braces balanced. Zero code references to `CMD_CHECK_TT`.
Include reduction −6 headers.

**Version bump:** v1.1.1 (versionCode 131) → v1.1.2 (versionCode 132).

### v1.1.1

**Security hardening (2026-07-31).**

- `jni/ternak-tt.cpp` — `run_bin()` uses `execv(2)` (no shell), so classic
  shell-metachar injection was never possible. However, `wipe_tt_data()` fed
  **unvalidated package names** from `target.txt` into
  `execv("/system/bin/pm", {"pm", "clear", pkg, ...})` and the equivalent
  `am` call while running as **root**. A malicious or typo'd line like
  `--user 0` or `com.android.settings` could inject a flag into `pm`/`am`
  or wipe data of an unintended package.
- **Fix:** added `is_valid_android_package()` (strict Android package-name
  grammar `[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z][a-zA-Z0-9_]*)+`, length ≤ 255,
  rejects `-`/`.` prefix, rejects empty segments, rejects trailing dot).
  Applied at two layers: `load_targets()` filters invalid lines with stderr
  warning, and `wipe_tt_data()` re-validates at the call site.

### v1.1.0

**Refactor pass (2026-07-29).**

Bug fixes surfaced during refactor:

1. `MOUNTDIR` + `BindEntry` + `BIND_ENTRIES[]` were duplicated across
   `main.cpp` and `companion.cpp` with 6 vs 9 entries out of sync. Unified
   into `jni/mount_targets.hpp`.
2. `install_gaid_hook()` was **dead code** — FindClass + DeleteLocalRef with
   no RegisterNatives. Now performs a real JNI hook on
   `AdvertisingIdClient$Info.getId()` and `isLimitAdTrackingEnabled()`,
   returning the identity's `GOOGLE_AID`.
3. `install_proc_sanitizer()` was defined but never called. Now invoked
   from `postAppSpecialize()`.

Refactors (8 functions previously flagged >50 lines):

- `hook_prop_get` (89L) → ~25L, tables in `jni/prop_map.hpp`.
- `generate_mount_files` (130L) → 5 helpers, table-driven from
  `PARTITIONS[]` in `mount_targets.hpp`.
- `apply_native` (126L) → `build_resetprop_list()` +
  `run_resetprop_batch()` + `apply_settings_puts()`.
- `build_variant` (67L) → four helpers.
- `set_gaid_value`, `rotate_bluetooth_mac`, `sync_device_name` → shared
  `bt_config_rewrite_field()` helper in `helpers.sh`.
- `do_mounts_via_fork` → `child_do_binds()` helper.

**Version bump:** v1.0.23 (versionCode 120) → v1.1.0 (versionCode 130).

### v1.0.23

See git log on `main` for the v1.0.x incremental history (README rewrite,
WebUI, auto-lock, CI auto-bump, etc.).
