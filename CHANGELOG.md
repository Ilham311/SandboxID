# Changelog

All notable changes to Ternak TT are recorded here. The GitHub Actions workflow
reads the matching `## vX.Y.Z` section to build `release_notes.md` automatically
on every release.

Format loosely follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and [Semantic Versioning](https://semver.org/).

---

## v1.1.0

### Added

- **Kernel identity bind (Path A)** — companion now bind-mounts two additional overlay files into the target's mount namespace:
  - `/proc/uptime` → random value between 1 hour and 30 days (fresh persona shouldn't look like a device that was just booted or one that's been up for months)
  - `/proc/sys/kernel/random/boot_id` → fresh UUIDv4 per identity rotation (SafetyNet + several fingerprinters cross-check this against `ro.boottime.zygote`)
- **Rich BIND-FAIL diagnostic** — when a bind mount fails post-`setns`, companion now logs source and destination `stat()` (inode / size / mode), the raw `errno` **plus** `strerror(errno)`, and the target pid, so root-cause is obvious without reproducing under strace. Example:
  ```
  BIND-FAIL /data/adb/modules/ternak_tt/mount/settings_secure.xml -> /data/system/users/0/settings_secure.xml \
    errno=2(No such file or directory) src{stat=0 ino=12345 size=421 mode=0100644} \
    dst{stat=-1 ino=0 mode=00} [post-setns pid=23811]
  ```
- **Path B: Java method hook scaffold (lsplant)** — new `jni/java_hooks.{hpp,cpp}` implements a lsplant-based hook framework, guarded by `TT_HAVE_LSPLANT`. When compiled with lsplant available, `postAppSpecialize` now calls `java_hooks::Init(env)` → `InstallAll(env, identity)` after the crash watchdog. **v1.1.0 ships the scaffold only** — `Init()` wires up the lsplant runtime, `InstallAll()` is a logging no-op. v1.1.1 will populate hooks for `Settings.Secure.getString` (android_id / bluetooth_address / advertising_id), `MediaDrm.getPropertyByteArray` (widevine deviceUniqueId), `Locale.getDefault`, `TimeZone.getDefault`, `SystemClock.uptimeMillis`, and `SystemClock.elapsedRealtime`.
- **`fetch_lsplant.sh`** — one-shot script that clones LSPosed/LSPlant + jmpews/Dobby into `jni/`, then (if `javac` + `d8` are on PATH) compiles `java_helper/TernakHookHelper.java` into `jni/helper_dex.h` for embedding.
- **CI workflow auto-fetch** — GitHub Actions runs `./fetch_lsplant.sh` after the Zygisk header fetch. Marked `continue-on-error: true`, so if fetch fails the module still builds — just with Path B disabled.
- **Conditional CMake integration** — `jni/CMakeLists.txt` now detects `jni/lsplant/` + `jni/dobby/` and, when both exist, adds them as subdirectories, sets `-DTT_HAVE_LSPLANT=1`, and links against `lsplant` + `dobby` + `dl`. Falls back to the original single-target build when either is missing.
- **`build.sh` Path B status message** — prints whether Path B is active or disabled before invoking cmake, so build output is self-documenting.
- **`java_helper/TernakHookHelper.java`** — scaffold Java class with `native` method declarations that the v1.1.1 hooks will point to for calling the original ART method via lsplant's backup handle.

### Fixed

- **BIND-FAIL log ambiguity from v1.0.18** — previously the log line was just `child: bind fail <src> -> <dst> errno=<n>`, which didn't distinguish "source file gone" from "target NS already has this path overlaid" from "selinux denied". The rich diagnostic above resolves this.

### Known limitations

- **Path B is scaffold-only in v1.1.0.** `InstallAll()` returns without hooking any Java methods. `Settings.Secure.getString("android_id")` and `MediaDrm.getPropertyByteArray("deviceUniqueId")` will still return the real device values on v1.1.0. Full hook bodies land in v1.1.1.
- **Path B build requires internet on CI** — `fetch_lsplant.sh` clones from GitHub. Offline / air-gapped builds must vendor `jni/lsplant/` + `jni/dobby/` manually.
- **`/proc/uptime` bind** may fail on some kernels that mark `/proc/uptime` as a synthetic pseudo-file rejecting bind sources; this is why bind failures now log `errno=EINVAL(Invalid argument)` explicitly.

### Unchanged

- L1 `Build.*` ×17 hooks, L2 native_get 42 identity keys + 13 static defaults, L6 Telephony ×4 hooks, L7 SPB ×9 / SPI ×18 / SPL ×1, watchdog ×4 signals — all identical to v1.0.18.
- Companion wire protocol, target.txt hot-reload, `ternak-tt targets` CLI, `summarize.sh` two-section output — unchanged.

### Impact

- **/proc/uptime + /proc/sys/kernel/random/boot_id** now spoofed. Fingerprinters that cross-check these against `ro.boottime.zygote` or `ro.build.date.utc` should no longer flag inconsistency.
- **SSAID, GAID, App Set ID, MediaDRM ID still leak on v1.1.0.** These identifiers are read via binder IPC to `system_server` / Play Services, so no mount-namespace overlay can intercept them. Path B in v1.1.1 will close this gap by hooking the ART methods directly in the target process.
- No regression on TT / Grab / Shopee flows expected — all v1.0.18 behavior preserved; new code is additive.

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
