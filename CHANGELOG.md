# Changelog

All notable changes to Ternak TT are recorded here. The GitHub Actions workflow
reads the matching `## vX.Y.Z` section to build `release_notes.md` automatically
on every release.

Format loosely follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and [Semantic Versioning](https://semver.org/).

---

## v1.0.30 (Unreleased)

Brings the **non-gaming, stealth** half of [COPG](https://github.com/AlirezaParsi/COPG)'s
feature set to Ternak TT's fresh-persona model. Everything added here stays inside
the module's existing safety envelope (safe identity strings, per-app scope, no
resident inline hooks), so boot risk is unchanged. See
[`docs/COPG-PARITY-v1.0.30.md`](docs/COPG-PARITY-v1.0.30.md) for the full research
write-up and the list of COPG features intentionally **not** ported.

### Added

- **Region persona — Timezone** (`jni/main.cpp`, `jni/ternak-tt.cpp`): each target app now reads a persona timezone. Applied per-app only via `java.util.TimeZone.setDefault`, the C-library `TZ`/`tzset`, and the existing `persist.sys.timezone` `native_get` hook — the real device timezone is never touched. Persona key `TIMEZONE` (default `Asia/Jakarta`).
- **Region persona — Locale / Language** (`jni/main.cpp`, `jni/ternak-tt.cpp`): per-app locale via `java.util.Locale.setDefault(Locale.forLanguageTag(...))`, plus `ro.product.locale[.language|.region]` and `persist.sys.locale` in the app-scoped `build.prop` overlay and `native_get` hook. Persona keys `LOCALE` (BCP-47, default `id-ID`), `LOCALE_LANG`, `LOCALE_COUNTRY`.
- **Region persona — SIM carrier** (`jni/pool_tt.hpp`, `jni/ternak-tt.cpp`): `freshen` now picks a region-consistent carrier (Indonesian pool) and wires it into the already-present `gsm.*operator.*` `native_get` hooks. Persona keys `GSM_OPERATOR_ALPHA`/`_NUMERIC`/`_ISO` (previously hard-coded fallbacks, now persona-driven).
- **SoC device fields** (`jni/pool_tt.hpp`, `jni/main.cpp`, `jni/ternak-tt.cpp`): `Build.SOC_MANUFACTURER` / `Build.SOC_MODEL` (API 31+) and `ro.soc.manufacturer` / `ro.soc.model` are now spoofed to the correct Tensor generation for each Pixel in the pool.
- **Opt-in fake uptime** (`jni/main.cpp`, `jni/ternak-tt.cpp`): when persona key `FAKE_UPTIME_MS` is set (>0), the three `android.os.SystemClock` readers (`uptimeMillis`, `elapsedRealtime`, `elapsedRealtimeNanos`) are re-implemented via `clock_gettime` + a constant offset, so an app distrusting a freshly-reset device sees a longer uptime. Off by default and reset by `freshen`. `currentTimeMillis` is deliberately left alone (shifting wall-clock breaks TLS).
- **`ternak-tt set <KEY> <VALUE>`** (`jni/ternak-tt.cpp`): runtime upsert for the region/uptime persona fields, preserving all other keys (including shell-owned `WIFI_MAC` / `BLUETOOTH_*`). Allowlisted keys only; validates `FAKE_UPTIME_MS` is numeric.
- **WebUI "Region" tab** (`webroot/`): edit timezone, locale, SIM carrier (name / MCC+MNC / ISO) and fake uptime with no reboot; the Persona tab now also shows SoC, timezone, locale, carrier and fake-uptime.

### Notes

- New spoofing surfaces are all **stealth / per-app scoped** and use the same `RegisterNatives`-on-native-methods + Java static-field techniques already in the module. No new native dependency, no inline/PLT hooking, no anti-cheat-unsafe resident hooks were introduced.
- **Skipped by request (gaming) and by design (needs risky resident hooks):** GPU spoof, display refresh-rate spoof, CPU-flagship/`/proc/cpuinfo` spoof, DRM/Widevine level, WebView User-Agent, IMEI/Global-IMEI, App Set ID, VPN-hide, mock-location-hide, hide-developer-options, and the DND / brightness / keep-screen-on / DPI comfort tweaks. Rationale in the parity doc.

---

### Fixed

- **Dead proc-sanitizer subsystem removed** (`jni/main.cpp`): `install_proc_sanitizer` and its `openat` / `__openat` / `android_get_device_api_level` PLT hooks, `find_libc_dev_inode`, and the `memfd_create` helper were **never wired into the Zygisk lifecycle** (zero call sites). They shipped in every build as pure dead code. Removed to shrink the module and cut maintenance/attack surface; also retires the memfd file-size fingerprint anomaly (old issue #28).
- **Unused mount tables removed** (`jni/main.cpp`): the `MOUNTDIR` constant and a duplicate `BIND_ENTRIES[]` table were unreferenced — the authoritative copy lives in `jni/companion.cpp`.
- **Companion mount child made async-fork-safe** (`jni/companion.cpp`): `do_mounts_via_fork` forked from a process that can already be running the death-reaper thread, then called `std::string` / `__android_log_print` in the child — a fork-in-multithreaded-process deadlock hazard. The child now uses only stack buffers and raw syscalls and reports counters over the pipe; all logging moved to the parent. Mount semantics and log-line formats are unchanged.
- **Debug typed-hook `SUPPRESS` leak** (`jni/main.cpp`): `native_get_long` / `native_get_boolean` read the real system property *before* labelling the key `SUPPRESS`, leaking the value they were meant to suppress. They now skip the read, matching `native_get_int`.
- **Version strings unified** (`jni/ternak-tt.cpp`, `customize.sh`, `webroot/index.html`): the CLI banner (was `v1.0.1`), synthetic `build.prop` header (was `v1.0.3`), install banner (was `v1.0.19`), and WebUI header (was `v1.0.19`) drifted from `module.prop`. They now derive the version from `module.prop` instead of hardcoding it.
- **`generate_uuid` fallback emitted a malformed UUID** (`helpers.sh`): the non-`/proc` path skipped a hex digit and never set the RFC 4122 variant nibble. Fixed to produce a conformant v4 UUID.
- **Release notes "What's new" was always empty** (`.github/workflows/build.yml`): the changelog extractor searched for `### vX.Y.Z` headings and required an exact line match, but this file uses `## vX.Y.Z` with `(Unreleased)` / date suffixes. The fix (match `## vX.Y.Z` by prefix) is provided as a patch in the PR description for a maintainer with `workflow` token scope to apply, since automation tokens cannot modify workflow files.
- **CHANGELOG structure** (`CHANGELOG.md`): removed a duplicate `# Changelog` H1 and restored the orphaned `v1.0.19` section to chronological order.
- **Minor**: removed a `cat | awk` (UUOC) in `service.sh`.

### Notes

- No new spoofing surfaces or identifiers were added. This release is a correctness, safety, and hygiene pass only.

---

## v1.0.28 (Unreleased)

### Removed

- **L3/L4/L5/L6 hooks removed**: The handlers and install stubs for `Settings.Secure`, `AdvertisingIdClient`, `WifiInfo`, and `TelephonyManager` were dead code (never registered with `RegisterNatives`). Removed to decrease risk; may be reintroduced properly in a future release.

### Fixed

- **CRLF sweep**: Fixed stray carriage returns across the repo.
- **Shell script quoting**: Fixed variables in `customize.sh`, `service.sh`, `summarize.sh`, and `action.sh` to prevent word splitting.
- **`action.sh` PIPESTATUS**: Replaced PIPESTATUS bashism with a reliable tempfile pattern for ash/mksh compatibility.
- **`post-fs-data.sh`**: Resolved `ro.product.cpu.abi` correctly and added a timeout guard to the seed binary call.
- **`service.sh`**: Replaced unbounded boot loop with max iterations limit, cleared `logcat.pid`/`journal.pid` upon start, and removed `logcat -c`.
- **`rotate_ids.sh`**: Added a plaintext check to skip encrypting `bt_config.conf` natively on newer OSs, and autodetected the `wlan` interface.
- **`customize.sh`**: Added unknown ABI abort, conditional binary permission application, and added NeoZygisk and ZygiskOnKernelSU paths.
- **`helpers.sh`**: Prevented `setprop` from trying to set `ro.*` keys without `resetprop-rs`.
- **`uninstall.sh`**: Added uninstall cleanup that safely manages log removal and backups display without deleting user/system data.

---

## v1.0.19 (2026-07-27)

### Action button is now 1-tap ready
- `action.sh` now runs **`bin/ternak-tt freshen` → `rotate_ids.sh all`** in sequence.
- Before v1.0.19 you had to `sh rotate_ids.sh all` manually after tapping Action. That step is gone — one tap in KernelSU/Magisk = fresh persona applied end-to-end.

### New: `rotate_ids.sh` + `helpers.sh`
- **helpers.sh** — shared shell library: `log_*`, refcounted `se_permissive`/`se_restore`, `get_users`, `generate_uuid`, `generate_mac`, `settings_put`, `rp_set`, `force_stop`, `identity_get`, `identity_persist`, `backup_rotate`.
- **rotate_ids.sh** — CLI dispatcher with `all` / `safe` / `ssaid` / `gaid` / `wlan-mac` / `bt-mac` / `device-name` / `status` / `help`.

### Design fix: `device_name` now consistent with hook persona
- Old `randomize_device_name` picked a random Brand+Model from a hardcoded list (`"Galaxy S24-427"`). That name clashed with the persona chosen by the L1/L2 hooks (which reads `MODEL` from `identity.prop` — e.g. `Pixel 8`).
- New `sync_device_name` reads `MODEL` from `identity.prop` and applies **that same value** to `settings put global device_name`, `settings put global bluetooth_name`, `persist.bluetooth.adaptername`, and rewrites `Name = ` in `bt_config.conf`. Hook layer and shell layer now report identical values.
- Optional override: `BLUETOOTH_NAME` key in `identity.prop` (if set, wins over `MODEL`).

### New: Bluetooth MAC rotation
- `rotate_bluetooth_mac()` writes `persist.service.bdroid.bdaddr`, `persist.sys.bt.bdaddr`, `persist.bluetooth.bdaddr`, `bluetooth.device.mac.address`, `ro.boot.btmacaddr`.
- Rewrites `Address = ` line in `/data/misc/{bluedroid,bluetooth}/bt_config.conf` and `/data/vendor/bluetooth/bt_config.conf`. If missing, injects under `[Adapter]` section.
- MAC persisted to `identity.prop` as `BLUETOOTH_ADDR` for reboot-idempotent state.

### Identity persistence
- `rotate_ids.sh` now writes `WIFI_MAC`, `BLUETOOTH_ADDR`, `BLUETOOTH_NAME` back into `identity.prop` via `identity_persist()` (atomic awk upsert). Next `freshen` preserves them; next `rotate all` reads them back — same value across reboots until an explicit re-rotate.

### Backup rotation
- All destructive ops (`wipe_ssaid`, `set_gaid_value`, `randomize_wlan_mac`, `rotate_bluetooth_mac`, `sync_device_name`) copy the target file into `$MODDIR/backups/` first.
- `backup_rotate PREFIX KEEP` prunes older files, keeping the newest N (defaults: SSAID/BT-config = 10, WifiConfigStore = 5).

### GAID hardening
- Now reads `GOOGLE_AID` from `identity.prop` (populated by `freshen`) instead of always regenerating. If missing, generates + persists.
- Guards against GMS not installed — writes to `Settings.Global` and returns without touching the shared_prefs dir.
- Optional `chcon` re-labels `adid_settings.xml` from parent directory context if `chcon` is available.

### Full flow (1-tap)
1. `bin/ternak-tt freshen` — rolls `MODEL`, `DEVICE`, `BRAND`, `SERIAL`, `ANDROID_ID`, `GOOGLE_AID`, etc.
2. `wipe_ssaid` — deletes `settings_ssaid.xml` per user (needs reboot to regenerate).
3. `set_gaid_value` — syncs `GOOGLE_AID` to `Settings.Global.advertising_id` + GMS `adid_settings.xml`.
4. `randomize_wlan_mac` — wlan0 MAC + wipes `WifiConfigStore.xml`.
5. `rotate_bluetooth_mac` — BT adapter MAC + `bt_config.conf` Address.
6. `sync_device_name` — device_name/bluetooth_name/`bt_config.conf` Name = `identity.prop` MODEL.

---

## v1.0.18

### Added

- **L2 native_get static defaults map** — 13 new keys that leaked in real-world sessions now return safe generic values instead of the device's real value: `gsm.operator.isroaming` ("false"), `ro.zygote` ("zygote64_32"), `ro.hardware` ("qcom"), `ro.board.platform` ("sm8250"), `ro.dalvik.vm.native.bridge` ("0"), `ro.allow.mock.location` ("0"), `dalvik.vm.isa.arm64.variant` / `.features`, `dalvik.vm.isa.arm.variant` / `.features`, `dalvik.vm.heapsize` ("512m"), `ro.build.version.preview_sdk` ("0"), `persist.radio.multisim.config` fallback ("ss"). Logged as `L2 SPOOF-STATIC` so they can be counted separately.
- **L2 identity-typed additions**: `ro.build.user`, `ro.build.host`, `ro.build.tags`, `ro.build.type` now hook via native_get (previously only spoofed via resetprop on `apply-boot`).
- **L7-SPB additions** (3 keys, from real leak trace): `persist.sys.activity_anim_perf_override` (was leaking 114× per session), `persist.sys.lmk.reportkills`, `debug.layout`.
- **L7-SPI addition**: `debug.adservices.binder_timeout` = 10000.

### Fixed

- **`summarize.sh` “Target packages seen” section was blank** even when the companion accepted target spawns. Root cause: the regex captured `pkg=` from every log line including REJECTs, then sort/uniq buried target matches. Now split into two sections: **“Target packages seen (ACCEPTED by companion)”** parsed from `ACCEPT pkg='...'` lines only, and **“All packages spawned (top 20, incl. rejected)”** for whitelist tuning context.

### Unchanged

- Companion bind-mount errno=2 seen on some post-`freshen` spawns is under investigation (write path is already atomic via `rename(2)`; likely mount(2) fails post-setns because target NS has an existing overlay). Added no runtime change in v1.0.18; richer per-mount diagnostic planned for v1.0.19.

### Impact

- Expected LEAK count in next debug session: ~5 (from 182 in v1.0.17). Remaining leaks: rare device-specific props not yet catalogued.

---

## v1.0.17

### Changed

- Migrated repository from `diru768/ternak-tt` to `Ilham311/Tt`. All README badges, install links, `git clone` URL, `module.prop` `updateJson`, and `update.json` seed now point to the new repo.
- `LICENSE` copyright reassigned to `Ilham311` (MIT).

### Fixed

- Placeholder text like `<ts>`, `<part>`, `<timestamp>` were being silently stripped by Markdown / web upload paths, leaving broken filenames such as `session-.log` in README, CHANGELOG, and `customize.sh`. All placeholders replaced with concrete stripping-proof forms: `session-YYYYMMDD-HHMMSS.log`, `summary-YYYYMMDD-HHMMSS.txt`, `/{partition}/etc/build.prop`.
- `module.prop` author reassigned from `diru768` to `Ilham311`.
- `customize.sh` install banner bumped from v1.0.15 to v1.0.17 (was 2 versions stale).

### Unchanged

- No functional / runtime behavior changes. Zygisk hook layers, companion IPC protocol, `target.txt` whitelist, crash watchdog, and summarizer all identical to v1.0.16. Safe to flash over v1.0.16 without state reset.

---

## v1.0.16

### Changed

- All source files (`.cpp`, `.hpp`, `.sh`, `CMakeLists.txt`, workflow YAML) stripped of comments for cleaner distribution. Total source size reduced ~22%.
- README fully rewritten with centered header, 5 status badges, table of contents, ASCII architecture diagram (boot flow + per-app-spawn flow), Requirements table, Command reference table, Env override table, and dedicated **Auto-release pipeline** section.
- New `CHANGELOG.md` (this file) added as canonical release history in Keep-a-Changelog format.

### Added

- `.github/workflows/build.yml` fully automated: reads `module.prop` → auto-bumps patch on tag collision → builds both variants → auto-generates `release_notes.md` from `CHANGELOG.md` section + git log since prev tag → auto-generates `update.json` from `module.prop` + repo slug → commits refreshed `update.json` + synced `module.prop` back to `main` → publishes GitHub Release. **Zero manual input.**
- `release_notes.md` and `update.json` are now build artifacts / release assets, no longer committed manually.

---

## v1.0.15

### Added

- **Runtime whitelist `target.txt`** at `/data/adb/modules/ternak_tt/target.txt`. Add / remove target packages without rebuilding.
- Companion loads and **hot-reloads** `target.txt` on mtime change (next app spawn picks up edits).
- New CLI subcommand `ternak-tt targets` to dump the active whitelist.
- L7 `SUPPRESS` label for known log-noise keys (`log.looper.*.slow`, `debug.watson.*`) to keep summaries readable.
- `summarize.sh` now breaks SPOOF hits out by hook layer (`L1` / `L2` / `L7-SPB` / `L7-SPI` / `L7-SPL`) and counts `SUPPRESS`, `REJECT`, `ACCEPT` separately.
- `customize.sh` **preserves** existing `target.txt` across reinstalls.

### Changed

- Zygisk companion IPC protocol for `CMD_GET_IDENTITY` now includes the pkg name; companion responds with `len=0` for non-targets (single source of truth for whitelist).
- Zygisk `.so` no longer contains a hardcoded target list.
- `ternak-tt.cpp` `wipe_tt_data()` reads targets from `target.txt` for symmetry with the Zygisk side.

### Fixed

- Whitelist drift between the CLI (`ternak-tt`) and Zygisk companion — both now share one file.

---

## v1.0.14

### Added

- `post-fs-data.sh` + new `ternak-tt seed` subcommand that generates identity + mount overlay files **before** Zygisk loads, fixing the first-boot race where the first TT/Grab pid got 0/6 bind mounts.
- Android 11+ canonical partition paths in `BIND_ENTRIES` (`/odm/etc/build.prop`, `/product/etc/build.prop`, `/system_ext/etc/build.prop`) alongside the legacy paths.
- Skip counter is split into `skip_src` (module bug) vs `skip_dst` (device doesn't have that partition — expected).

### Fixed

- 3-skip on POCO F3 / MIUI-style ROMs where partition build.prop lives at `/{partition}/etc/build.prop`.

---

## v1.0.13

### Added

- **Per-type L7 spoof maps** (`tt_bool_spoof`, `tt_int_spoof`, `tt_long_spoof`) consulted by the typed `native_get_*` hooks before falling back to `def`.
- Critical spoof: `sys.boot_completed = true` (previously returned `false`, breaking app boot-detection retry loops).
- L2 `debug.force_rtl` → `false`.

### Fixed

- 800+ hot L7 leaks from v1.0.12 telemetry (`sys.boot_completed`, `debug.sqlite.*`, `build.version.extensions.*`, `ro.gfx.driver_build_time`, `dalvik.vm.dexopt.secondary`, etc.).

---

## v1.0.12

### Added

- Crash watchdog rewrite: 4 signals (`SIGABRT` / `SIGFPE` / `SIGILL` / `SIGSYS`), rate-limited 3 per signal, restores `SIG_DFL` (no signal chaining).
- 18 additional L2 `native_get` spoofs (`gsm.*`, `sys.boot_completed`, `cpu.abi*`, `dalvik.vm.heapgrowthlimit`, `ro.build.characteristics`, `persist.sys.timezone`, `ro.mediacodec.*`).

### Fixed

- YAML workflow version resolution (was overwriting `module.prop` with git-describe fallback `v0.0.6`).
- `summarize.sh` MOUNT regex missing the new companion log format.

---

## v1.0.11

### Added

- Standalone `summarize.sh` that condenses a 7 MB session log into a ~10 KB chat-shareable summary.
- Action tap on debug variant now auto-produces `summary-YYYYMMDD-HHMMSS.txt`, copies `crashes.log`, and gzips the raw log to `/sdcard/Download/ternak-tt-logs/`.
- Automatic pruning: keeps newest 10 summaries / crashes / raw.gz per install.

---

## v1.0.10

### Added

- **Zero-setup auto-log capture** on debug variant. `service.sh` starts a background logcat on boot into `/data/adb/modules/ternak_tt/debug/session-YYYYMMDD-HHMMSS.log`, keeping the 5 newest sessions.
- Session header written to each log (module version, boot time, uptime, Android SDK, device, ABI, installed root modules).

---

## v1.0.9

### Added

- Detailed `[D]` traces for every hook layer (which key was queried, what was returned, LEAK vs SPOOF vs MISS labelling).
- Companion mount timeline traces (`setns OK`, per-bind check, ok/fail/skip breakdown).
- Crash / death / leak journal that persists across reboots.

---

## v1.0.8

### Added

- Debug variant build alongside release. Both variants are produced by `build.sh` per invocation.
- `TT_DEBUG` compile-time flag: release strips `LOGD` calls entirely (zero cost), debug keeps them.

---

## v1.0.7

### Added

- Expanded target set to include Grab Passenger (`com.grabtaxi.passenger`) — driver/passenger apps do heavy device fingerprinting for fraud.

---

## v1.0.6

### Fixed

- Bind-mount `EINVAL` in the Zygisk-Next companion. Companion now forks a single-threaded child that `setns`es into the target's mount namespace before mounting.

---

## v1.0.5

### Added

- Zygisk companion process serving `CMD_GET_IDENTITY` and `CMD_DO_MOUNTS` over the built-in Zygisk UDS.

---

## v1.0.4

### Fixed

- `EACCES` on `mount()` from `preAppSpecialize` (CAP_SYS_ADMIN dropped). Mounting is now handled by the companion, which retains caps.

---

## v1.0.3

### Added

- Bind-mount overlay tree at `$MODPATH/mount/{system,vendor,odm,product,system_ext}/build.prop` + `settings_secure.xml`.
- `ternak-tt freshen` regenerates all overlay files.

---

## v1.0.2

### Added

- `resetprop-rs` invocation from `apply-boot` and `freshen` to broadcast native property changes.

---

## v1.0.1

### Added

- Standalone CLI (`ternak-tt`) with `freshen`, `status`, `rollback`, `lock`, `unlock`, `apply-boot` subcommands.

---

## v1.0.0

Initial release.

- 6-layer Java hook: `Build.*`, `SystemProperties.native_get`, `Settings.Secure.getString`, `AdvertisingIdClient.Info.getId` (stub), `WifiInfo.getMacAddress` / `getBSSID`, `TelephonyManager.getImei` / `getDeviceId` / `getSubscriberId` / `getMeid`.
- Pixel-only device pool (SDK 33–36).
- TikTok Global / Asia / Lite target packages.
