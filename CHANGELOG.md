# Changelog

## Unreleased

### ByteDance AppLog IDs: in-process hook (L9) replaces disk seeding

`did` / `iid` / `ssid` / `openudid` / `clientudid` / `cdid` are now spoofed
in-process by the zygisk module instead of being seeded onto the app's data
dir. The old seeding needed `setenforce 0`, `chown`, `restorecon` and a
force-stop, and lost to the SDK's own writes; the hook is authoritative on
every read.

Formats are grounded in observed `device_register` traffic (real samples:
`6990234216324986369 >> 22` = 2022-10-24; iid `7137846409338136325` likewise
decodes to its registration date), not the previously-assumed
`(unix_seconds << 32)` shape:

- **`did` / `iid` / `ssid`** — int64 snowflake, 19 decimal digits,
  `(unix_ms << 22) | 22-bit random`, per package (each ByteDance app
  registers its own did)
- **`cdid` / `clientudid`** — UUID v4
- **`openudid`** — 16 hex chars
- **`APPLOG_EPOCH`** (identity.prop) — new key; all six IDs are
  deterministic in (persona, package, epoch), so bumping the epoch rotates
  them. `rotate_ids.sh applog` does exactly that (bump + wipe + force-stop);
  values stay stable between bumps.
- **XML patching** — `applog.xml` / `snssdk_*.xml` / `bd_device_info.xml`
  are read from disk and only identifier-matching entries are patched,
  preserving the SDK's own key names (`device_id`, `header_device_id`, …
  vary across versions). Raw `bd_setting/*` + `.cdid` are served
  synthesized. Known limitation: MMKV-backed stores (read-write mmap) are
  not covered.
- **Removed** — `helpers.sh::applog_generate` (incl. ~60 lines of awk
  decimal long-arithmetic and a bogus `arxiv:2504.13279` citation — that
  paper is about TikTok post sampling, not ID formats) and
  `helpers.sh::applog_seed`. `applog_probe` states simplify to
  fresh/active/absent.
- **Tests** — `tests/native_read_test.cpp` gains classify/synthesize/patch
  coverage (262 checks total), including snowflake decode round-trip and
  idempotence of the XML patcher.

### Review fixes (shell)

- `rotate_ids.sh all/safe` — the optional GAID argument leaked into
  `sync_device_name` (a UUID could become the device name); only
  `set_gaid_value` receives it now.
- `rotate_ids.sh all` — `randomize_wlan_mac` failures now count toward
  `FAILURES` like BT-MAC already did.
- `action.sh` — rotation warnings now surface every nonzero rc (rc=1, the
  common failure code, was explicitly suppressed).
- `jni/main.cpp` — fixed latent build break from `bool ok =
  api->hookJniNativeMethods(...)`: the pinned zygisk.hpp API returns void
  (failure is signalled by `fnPtr == null`).

### Fix: `action.sh` fails with rc=127 "binary native tidak bisa dijalankan"

`bin/sandboxid` does not exist in the zip — it is an install-time symlink
that `customize.sh` creates per ABI (`ln -sf sandboxid-arm64 …`). Installs
whose module dir was updated without a full re-flash (or that predate the
`ternak-tt` → `sandboxid` rename) can be missing it, which made
`action.sh` abort with rc=127 before applying any identity. All consumers
now fall back to the ABI-named binaries (`bin/sandboxid-{arm64,arm,x86_64,x}`
selected via `ro.product.cpu.abi`, `uname -m` as backup):

- `helpers.sh` gains `sbx_bin()` (also tries legacy `ternak-tt-<abi>` names);
  `action.sh` re-resolves `BIN` through it after sourcing helpers.
- `service.sh` and `post-fs-data.sh` resolve inline (they run standalone,
  before helpers.sh is sourced); the arm64-only fallback in
  `post-fs-data.sh` covered 32-bit devices incorrectly.
- `jni/companion.cpp` `try_seed_ondemand()` tries `bin/sandboxid` then the
  compile-time-ABI binary, with a clearer error when neither is runnable.
- `action.sh`'s failure message now lists what it looked for instead of a
  bare path, and warns early (before autopif) when no native binary is
  runnable at all.
- `webroot/app.js` — removed the unused `BIN` constant (dead code; the
  WebUI always goes through `action.sh`/`rotate_ids.sh`).

### Identity hooking-completeness pass (Phase 4, jni/)

`Build.TIME` was the last un-spoofed `Build` field and the typed
`SystemProperties.getInt/getLong` getters could still return the *real* device
value for keys the persona overrides. Both gaps are closed, and the autopif
(the primary `action.sh` path) now emits the same extended key set the C++
`freshen` path does, so all persona surfaces stay mutually consistent.

- **`Build.TIME` / `ro.build.date.utc` / `ro.build.date`** — identity now
  carries `BUILD_TIME_UTC` + `BUILD_DATE`, derived deterministically from the
  persona's security-patch bulletin date (builds are stamped 1–6 days before
  the bulletin, mirroring real Pixel build cadence). L1 sets the `TIME` static
  long field; L2 maps both props; `apply_native()` + `generate_mount_files()`
  + `autopif.sh` emit them.
- **Typed-getter identity consistency (L7)** — `native_get_int` /
  `native_get_long` now consult the identity map first: any spoofed prop whose
  value parses strictly as an integer returns the persona value, so
  `SystemProperties.getInt("ro.build.version.sdk")`,
  `getInt("ro.build.version.preview_sdk")`,
  `getInt("ro.odm.build.media_performance_class")` and
  `getLong("ro.build.date.utc")` no longer leak the real device values
  (previously only the String `native_get` path was spoofed).
- **`ro.build.flavor`** — new identity key `FLAVOR` (`<product>-<type>`, e.g.
  `oriole-user`). `Build.FLAVOR` is gone from the SDK but the property still
  ships on production builds and is read directly by fingerprint SDKs.
- **`ro.build.version.codename` / `all_codenames`** — pinned to `REL`
  (production value) in `STATIC_PROP_DEFAULTS`, `apply_native()` and the
  mount-overlay `build.prop` files.
- **`ro.product.{system,odm,product}.marketname`** — added to the L2 identity
  map, matching the per-part fallback chain the other product fields already
  had.
- **autopif.sh** — generated identity now also includes `VBMETA_DIGEST`,
  `FLAVOR`, `BUILD_TIME_UTC`, `BUILD_DATE` (was C++-`freshen`-only before;
  the primary path leaked the real build date and vbmeta digest).
- **`selftest.sh`** — new coherence checks #6 (`FLAVOR` == `PRODUCT-TYPE`) and
  #7 (`BUILD_TIME_UTC` numeric, within 2009→now) catch a broken persona before
  a target app sees it.

### Robustness

- **`main.cpp` L9 `fopen` fallback** — if the `fopen` PLT slot fails to
  resolve but `open`/`openat` hooks are live, `fopen()` is now served through
  `orig_openat` + `fdopen` with a proper mode→flags mapping. Previously the
  fallback path returned `ENOSYS`, which could break apps doing legitimate
  read *and write* `fopen()` calls after a partial hook commit.
- **`main.cpp` L2/L7 hook installation** — `hookJniNativeMethods()` return
  value is now checked (bool) in addition to the pending-exception check;
  failures are logged with layer tags instead of passing silently.
- **`companion.cpp`** — `target.txt` hot-reload now compares mtime with
  nanosecond resolution (`st_mtim.tv_sec/tv_nsec`); same-second edits are no
  longer missed by the seconds-only comparison.
- **Removed dead config `NO_TELEPHONY`** (`config.hpp`) — the constant and its
  comment described a TelephonyManager binder hook that was never implemented;
  nothing read it. Removing it prevents future confusion. GSM/SIM identity
  remains covered at the property layer (`gsm.*` spoof map + carrier merge).
- **`build.sh`** — fail-fast pre-flight for `cmake`/`zip`/`curl` (clear error
  instead of a mid-build crash), explicit error + hint on `zygisk.hpp` fetch
  failure, and `LICENSE` + `CREDITS.md` are now shipped inside every module
  zip for attribution transparency.
- **`jni/CMakeLists.txt`** — new `SBX_BUILD_TESTS` option wires the previously
  orphaned `tests/carrier_test.cpp` / `tests/native_read_test.cpp` into
  CTest for host builds.
- All jni sources verified against `@FastNative` vs `@CriticalNative` calling
  conventions in AOSP `SystemProperties.java` (string-keyed getters are
  `@FastNative`, so the `(JNIEnv*, jclass, …)` hook signatures are correct;
  handle-based overloads are `@CriticalNative` and deliberately not hooked) —
  documented in `CREDITS.md`.

Dynamic persona pool: the compiled Pixel table is replaced by a runtime data
file, with an optional live refresh from Google's canary build data. Persona
selection, SoC/RADIO derivation, and SDK-matching are all preserved — behavior
offline is identical to before.

ByteDance AppLog SDK **full regen** (wipe → generate → seed): the SDK's
server-issued identifier trio (`did` / `iid` / `ssid`) plus `openudid` /
`clientudid` / `cdid` is now not merely wiped but replaced end-to-end. A
locally-generated plausible cache lands on disk so the app reads the seeded
values on next cold start as if they were its own persistent state — no
zero-value gap, no "device changed" telemetry event on re-registration.
Format matches ByteDance's actual shapes (arxiv:2504.13279 + reverse-engineered
RangersAppLog paths): Snowflake 64-bit int64 (18-19 decimal digits, top 32
bits = Unix seconds) for did/iid/ssid; UUID v4 for cdid/clientudid; 16-hex
for openudid. Wires into the 1-click flow (`action.sh` → `rotate_ids.sh all`)
so a single tap in the WebUI or the Action button produces a fully-fresh
hardware + AppLog persona ready to use, with no user input. Ship-idle
contract preserved: no-op with a friendly hint when `target.txt` is empty.

### Added

- `helpers.sh::applog_generate` — plausible 6-tuple generator. Produces a
  Snowflake-shaped 64-bit int64 for did/iid/ssid using awk int64-safe math
  `(now_seconds << 32) | rand32` (three independent random halves so the
  three IDs never collide), a UUID v4 for cdid/clientudid from
  `/proc/sys/kernel/random/uuid` (with a `/dev/urandom` hand-rolled fallback
  when the kernel path is unavailable — musl/older kernels), and 16 hex
  chars from `/dev/urandom` for openudid. Values are printed to stdout as
  `KEY=value` lines; the caller is expected to redact them from logs (the
  helper itself never logs them).
- `helpers.sh::applog_seed [pkg] [payload]` — writes the fabricated cache
  into the target app's data dir. Emits five files: `shared_prefs/applog.xml`
  (primary cache: did/iid/ssid/openudid/clientudid/register_time),
  `shared_prefs/snssdk_openudid.xml` (legacy SDK path for openudid/clientudid),
  `shared_prefs/bd_device_info.xml` (RangersAppLog v6+ unified path for
  cdid/device_id), `files/bd_setting/{device_id, install_id, openudid,
  clientudid}` (raw text files read by native `libbdtracker.so` bypassing
  SharedPreferences), and `files/.cdid` (legacy plain-text UUID). Ownership
  chowned to the package UID (sourced from the data-dir itself), permissions
  set to 0660, and `restorecon -R` re-labels every seeded file so the app's
  SELinux domain can actually read them.
- `helpers.sh::applog_regen [pkg]` — the orchestrator: force-stop → wipe old
  cache (via applog_wipe) → generate new values → seed valid caches. Handles
  both single-package and `target.txt` batch modes. Per-target recursion so
  a partial failure on one package doesn't leave others in an inconsistent
  wipe-without-seed state.
- `helpers.sh::applog_probe [pkg]` — read-only status: emits one line
  `<pkg> <count> <state>` where state ∈ {fresh, seeded, active, absent}.
  Never dumps identifier values. Consumed by `rotate_ids.sh status`, by
  `action.sh` for the post-run summary, and by the WebUI applog card.
- `rotate_ids.sh applog [pkg]` (aliases `bytedance`, `regen-applog`) — front
  end for `applog_regen`. Full regen cycle by default.
- `rotate_ids.sh applog-wipe [pkg]` (alias `wipe-applog`) — escape hatch:
  wipe-only, no seed. Kept for forensic scenarios where you want to observe
  the SDK re-register from scratch against the server.
- WebUI: new "AppLog ByteDance" card in the Rotasi tab. Renders per-target
  state (seeded / aktif / bersih / nihil) plus file count, populated
  asynchronously via `applog_probe` so it never blocks the initial paint.
  Included in "Rotasi semua". Values are never surfaced — the card only
  shows counts and state tokens.
- README `applog` section rewritten with exact ID formats (Snowflake bit
  layout referencing arxiv:2504.13279, UUID v4 sources, 16-hex openudid),
  the five seeded files with per-file purpose, and the ownership + SELinux
  restorecon steps. Covered-scope entry upgraded from "cache wipe" to
  "full regen cycle".

### Changed

- `rotate_ids.sh all` and `rotate_ids.sh safe` now call `regen_applog`
  (was `wipe_applog`) as the **last** step. Ordering matters: force-stop
  inside applog_regen kicks each target so on next cold start it reads
  the seeded did/iid/ssid — and by that point every hardware-layer
  identifier it would sample (Build.*, MAC, ANDROID_ID, GAID) has already
  been rotated. If regen ran first, a stray user-initiated relaunch
  before the other rotations landed would burn our seeded IDs on top of
  the stale hardware fingerprint.
- `action.sh` now emits a post-run "Status AppLog per aplikasi target"
  block summarising each target's applog cache state (seeded / aktif /
  bersih / nihil) plus file count. Privacy-safe: never dumps identifier
  values.
- WebUI "Acak perangkat baru" button (freshenBtn) now reloads the Rotasi
  tab if it's currently active, so the AppLog card refreshes to show
  the seeded state immediately without a manual tab switch. Its inline
  comment now documents step 5 of the action.sh flow (AppLog wipe +
  generate + seed).
- `rotate_ids.sh status` per-target AppLog line now shows both file count
  AND state token (was file count only), sourced from applog_probe.


- `personas.tsv` (repo root, shipped to `/data/adb/modules/sandboxid/`) — the
  curated **stable** Pixel pool (the same 14 entries that were compiled into
  `jni/pool.hpp`), now editable data instead of C++. Tab-separated, 10 columns:
  `model device product board platform sdk release id incremental security_patch`.
  `#`-prefixed lines are comments. This file alone reproduces today's behavior.
- `autopif.sh` — best-effort refresher that scrapes Google's public Pixel pages
  for the latest **canary** build per model and *upserts* those personas into
  `personas.tsv` (dedupe by codename). Adapted from dannycreations' `autopif.sh`
  (see [CREDITS.md](./CREDITS.md)); rewritten for on-device Android `sh`.
  - **No-op offline:** exits 0 without touching anything when the device has no
    `curl`/`wget` (the usual case), so the bundled stable pool stays in force.
  - **SoC allow-list:** devices whose codename doesn't map to a known platform
    (`gs101`/`gs201`/`zuma`/`zumapro`/`laguna`) are skipped, never guessed, so a
    spoofed `RADIO` / `ro.board.platform` can't go inconsistent.
  - Wired into `action.sh` as a best-effort step 0 (before `freshen`); can also
    run at build time to refresh the packaged pool via `AUTOPIF_REFRESH=1`.

### Changed

- `jni/sandboxid.cpp` `gen_identity()`: loads the pool from `personas.tsv` at
  runtime (`load_personas()`), falling back to a small built-in list if the file
  is missing/empty. Selection logic (exact SDK match → `sdk<=dev` → lowest) is
  unchanged.
- `customize.sh` / `build.sh`: install and package `personas.tsv` + `autopif.sh`.

### Removed

- `jni/pool.hpp` — the compiled `SBX_POOL` table. Its data now lives in
  `personas.tsv`; `jni/config.hpp` gains `PERSONAS_FILE`.

## v2.1.0 (2026-08-26)

SIM / mobile-operator identity. Adds a user-controlled way to study the operator
fields apps read (`gsm.operator.*` / `Build`-adjacent SIM props), with an
optional "phantom" mode that reports a SIM as present on an empty slot. Like the
rest of the module this ships **inert**: no carrier is applied until the user
picks one, and clearing it falls straight back to the built-in defaults.

### Added

- `carriers.tsv` (repo root, shipped to `/data/adb/modules/sandboxid/`) — a
  curated table of **140 real operators across 67 countries** sourced from the
  public MCC/MNC dataset. Tab-separated, 4 columns: `name mcc mnc iso`;
  `#`-prefixed lines are comments.
- `carrier` (alias `sim`) subcommand in `rotate_ids.sh`:
  - `carrier "MCC|MNC|NAME|ISO|PHANTOM"` — validate (MCC = 3 digits, MNC = 2–3
    digits) and write `carrier.conf` (atomic, `umask 077`), then persist the
    `GSM_OPERATOR_NUMERIC/ALPHA/ISO` keys (and `GSM_SIM_STATE=LOADED` when
    `PHANTOM=1`) into `identity.prop`.
  - `carrier off` — clear the selection and erase the carrier keys.
  - `carrier status` — print the active selection.
- WebUI **SIM** tab (`webroot/index.html` + `app.js`): country/operator dropdowns
  driven by `carriers.tsv`, a "phantom" checkbox, and Apply/Off buttons that call
  the `carrier` subcommand and reflect the live `carrier.conf`.
- `jni/sbx_carrier.hpp` — pure, host-testable carrier-selection parsing
  (`parse_carrier_conf` + `apply_carrier`), split out so the shell and native
  paths agree on the "a valid selection sets the keys, anything else erases
  them" contract. Covered by `tests/carrier_test.cpp` (40 checks).
- New identity key `GSM_SIM_STATE` (default empty) with `gsm.sim.state` /
  `gsm.sim.state.ril` property mappings; `PHANTOM=1` maps it to `LOADED`.

### Changed

- `jni/sandboxid.cpp` `merge_carrier()`: honors `carrier.conf` on `freshen` and
  on first-boot `seed`, folding the parsed selection into the generated identity.
- `jni/main.cpp`: added `ro.vendor_dlkm.build.fingerprint` /
  `ro.odm_dlkm.build.fingerprint` → `FINGERPRINT` mappings so the newer
  partition fingerprints stay consistent with the rest of the build identity.
- `customize.sh` preserves a live `carrier.conf` across upgrades; `build.sh`
  packages `carriers.tsv`.

### Notes

- This is a **property-layer** path. SIM presence/state read through the
  telephony binder (`TelephonyManager.getSimState`, `SubscriptionInfo`) is **not**
  covered here and would need a separate framework-hook layer ("L3").
- Native changes require a CI rebuild + reflash; the pure carrier logic is
  verified on-host by `tests/carrier_test.cpp`.

## v2.0.0 (2026-08-21)

Rebrand to SandboxID — a generic, privacy-research / education framing with no
built-in application targets and no product-specific defaults. Runtime behavior
(hook layers, companion IPC, crash watchdog, atomic writes, hot-reload) is
unchanged; this release is naming, cleanup, and default-emptying only.

### Renamed

- Module id `ternak_tt` → `sandboxid`; module path `/data/adb/modules/sandboxid`.
- Display name `Ternak TT` → `SandboxID`; log tags `TernakTT`/`TernakTTCompanion`/`TernakTT-L3` → `SandboxID`/`SandboxIDCompanion`/`SandboxID-L3`.
- CLI binary `ternak-tt` → `sandboxid`; native library `libternak_tt.so` → `libsandboxid.so`.
- `jni/pool_tt.hpp` → `jni/pool.hpp`; `jni/tt_config.hpp` → `jni/config.hpp`.
- `jni/TtHook.java` → `jni/SandboxIDHook.java` (file layout only). The Java class
  inside is kept as the camouflaged `androidx.core.os.EnvCompatState` so a target
  app dumping loaded classes does not see an obvious anomaly.
- `jni/tt_lsplant.hpp` → `jni/lsplant.hpp`; generated DEX header `tt_hook_dex.h` →
  `hook_dex.h` (symbols `tt_hook_dex` / `tt_hook_dex_len` → `hook_dex` / `hook_dex_len`).
- `jni/ternak-tt.cpp` → `jni/sandboxid.cpp`.
- C++ namespace `tt::` → `sandboxid::`; `ttlsp::` → `sbxlsp::`; macro prefix `TT_` → `SBX_`.

### Changed

- `jni/companion.cpp` `reload_targets_if_changed()`: removed the built-in default target list; an absent `target.txt` now yields an empty list (no-op path).
- `jni/sandboxid.cpp` `load_targets()`: removed the default fallback; an empty `target.txt` returns an empty list.
- `jni/sandboxid.cpp` `usage()`: rewritten to a neutral description.
- `jni/sandboxid.cpp`: removed the unused `MODDIR` alias (fixes `-Wunused-const-variable` under `-Wall -Wextra`).
- `target.txt` now ships empty (0 lines) — the module is idle until the user adds packages.
- All comments stripped from source (`.cpp`, `.hpp`, `.java`, `.sh`, `.js`, `CMakeLists.txt`, workflow `.yml`); data literals (`SBX_POOL`, `VAL_DEFAULTS`, `STATIC_PROP_DEFAULTS`, `modem_prefix`) untouched.
- `jni/lsplant.hpp`: `kCls` and the `loadClass` string reference `androidx.core.os.EnvCompatState` (class name reverted to the camouflaged androidx-flavored name; `SandboxIDHook.java` keeps the new filename only).

### L3 — DEX header regeneration required before release

- The LSPlant L3 path is **default-OFF** (`SBX_ENABLE_LSPLANT`), so the release build
  is unaffected and an absent DEX header only produces the existing fail-safe
  (`LOGE` + continue; L1/L2 still apply).
- `jni/hook_dex.h` (the DEX bytecode for `androidx.core.os.EnvCompatState`) was not
  regenerated in this environment. **It must be regenerated before enabling L3**:
  `javac SandboxIDHook.java` → `d8` → `xxd -i` into `jni/hook_dex.h` so the embedded
  bytecode matches the current class name. Until then, L3 is **non-functional** and must
  not be enabled.
- No behavior change for the default (L3 off) runtime.

### Unchanged

- Hook layers, companion bind-mount, crash watchdog, atomic write, hot-reload, and CLI commands are identical to v1.0.36.

## Unreleased — L3 LSPlant foundation (scaffold, DEFAULT-OFF)

> **Catatan rilis:** perubahan di bawah **belum aktif di runtime**. Semuanya
> dikurung compile-flag `SBX_ENABLE_LSPLANT` (default OFF), jadi build release
> saat ini **byte-identik dengan v1.0.27**. `module.prop` **sengaja belum
> di-bump** — mem-bump ke v1.0.28 sebagai versi rilis berarti mengklaim
> kemampuan yang belum lolos verifikasi boot. Saat flag di-flip + lolos
> build+boot test, ubah heading ini jadi `## v1.0.28 (tanggal)` dan bump
> `module.prop` v1.0.27→v1.0.28 / versionCode 123→124.

### Added — L3: fondasi hook method Java non-native via LSPlant (`jni/tt_lsplant.hpp`)

- **Apa:** File wrapper baru `tt_lsplant.hpp` + call-site fail-safe di
  `postAppSpecialize` (`main.cpp`) + `option(SBX_ENABLE_LSPLANT)` di
  `CMakeLists.txt`. Default OFF → stub no-op, nol perubahan runtime. Saat ON:
  `sbxlsp::init()` menjalankan `lsplant::Init` (backend **Dobby** + resolver
  simbol libart), lalu `hook_android_id()` meng-ART-hook
  `Settings.Secure.getString(...)` agar app melihat `ANDROID_ID` persona yang
  deterministik.
- **Kenapa (rujuk trace Fase 2):** L2 (`hookJniNativeMethods`) hanya bisa
  mengikat method `native`; `Settings.Secure.getString` **bukan** native →
  butuh ART method-hooking. ANDROID_ID adalah identifier level-app terpenting,
  dan jalur file (mount `settings_secure.xml`) TIDAK andal untuknya: sejak API 26
  android_id ada di tabel per-app `ssaid`, bukan `settings_secure.xml` (itu
  sebabnya `rotate_ids.sh` harus `wipe_ssaid`). Hook method mem-*pin* nilai
  persis per-persona, bukan mengandalkan regenerasi acak.
- ⚠️ **BOOT RISK + mitigasi:** LSPlant menyentuh internal `ArtMethod`; kegagalan
  di ROM/versi tertentu = boot-loop **semua** app target. Mitigasi: (1) DEFAULT
  OFF via compile-flag → tak ada kode ART di build rilis saat ini; (2) call-site
  fail-safe — `Init`/`Hook` gagal ⇒ `LOGE` + lanjut, L1/L2/L7 tetap jalan, tak
  pernah `abort()`/unload setengah-jadi; (3) L3 dipasang **paling akhir** di
  `postAppSpecialize` sehingga layer terbukti terpasang lebih dulu.

### Notes — riset LSPlant (rujuk Fase 1)

- **Pin `v6.4`** (C++20 + header klasik). **Hindari `master`** (C++23 + C++
  Modules) — menaikkan bar toolchain tanpa manfaat untuk kasus ini.
- Backend inline-hook & resolver simbol **tidak dibundel** LSPlant — wajib
  disuplai lewat `InitInfo` (Dobby + lsparself/ElfImg, sesuai test resmi).
- Default `generated_class_name="LSPHooker_"` adalah fingerprint LSPosed →
  di-scaffold dengan nama netral (anti-tell).
- **Sengaja TIDAK di-hook** (alasan konkret): `WifiInfo.getMacAddress()` sudah
  dianonimkan `02:00:00:00:00:00` untuk app non-privileged (API 23+);
  `TelephonyManager.getImei/getSubscriberId/getSimSerialNumber` butuh
  `READ_PRIVILEGED_PHONE_STATE` (API 29+, permission signature) → app
  pihak-ketiga kena `SecurityException` sebelum body jalan, spoof di situ malah
  jadi anomali; `Build.getRadioVersion()` hanya membaca `gsm.version.baseband`
  yang sudah dicakup L2 + resetprop.

### Status SEAM & cara mengaktifkan (butuh build + boot test)

Kedua SEAM kini **sudah diimplementasikan sebagai kode** (bukan lagi placeholder):

- **SEAM #1 — resolver simbol libart:** di-wire ke **lsparself**
  (`lsparself::Elf("/libart.so")`), persis test resmi LSPlant v6.4. lsparself
  menangani `.gnu_debugdata` (mini-symtab LZMA) — sebabnya kita TIDAK hand-roll
  parser ELF naif yang akan diam-diam gagal menemukan simbol ART internal.
- **SEAM #2 — callback:** kelas Java `androidx.core.os.SandboxIDHook`
  (`jni/SandboxIDHook.java`) dengan `Object handle(Object[])` **pure-Java** (tanpa method
  `native`/RegisterNatives). Nilai spoof + `Method` asli disuntik dari native ke
  field statik; app dapat android_id persona untuk key `"android_id"`, dan nilai
  asli untuk key lain (chaining via backup). DEX-nya di-embed sebagai byte array
  (`jni/tt_hook_dex.h`) hasil `d8` atas `SandboxIDHook.java`; kalau header itu belum ada,
  `hook_android_id()` fail-safe (LOGE + lanjut).

Yang MASIH perlu dilakukan (semua butuh compiler + device — TIDAK tersedia di
lingkungan review ini, jadi belum diverifikasi):

1. Vendor `jni/external/{lsplant@v6.4 (--recurse-submodules), dobby, lsparself}`.
   (Submodule `lsparself`/`lsprism` LSPlant memakai URL SSH → di CI fetch lsparself
   via HTTPS terpisah.)
2. Bangun `jni/tt_hook_dex.h` dari `jni/SandboxIDHook.java`: `javac` → `d8` → `xxd -i`
   (simbol `tt_hook_dex` / `tt_hook_dex_len`).
3. `SBX_ENABLE_LSPLANT=ON ./build.sh` (build.sh sudah mengalirkan flag ke CMake;
   pertimbangkan `-DANDROID_STL=c++_static`), lalu **boot test per ABI/Android
   version** sebelum flag di-flip. `build.yml` sengaja TIDAK diubah (edit YAML buta
   berisiko memecah parsing CI produksi); langkah CI di atas ditambahkan saat enable.

### Changed — review pass (JNI local-ref hygiene + dokumentasi watchdog)

Pass review 6-gejala (mandiri, berbasis source-of-truth). 5 dari 6 gejala terbukti
false-positive / keep / defer; hanya **1** perbaikan kode nyata (hygiene, bukan crash)
+ **1** komentar. Build rilis **tetap byte-identik dengan v1.0.27** (perubahan hanya di
kode L3 default-OFF yang belum aktif + 1 komentar nol-efek di `main.cpp`).

- **`jni/tt_lsplant.hpp` (ON-branch, default-OFF) — JNI local-ref frame.**
  `load_callback_class()` & `hook_android_id()` kini membungkus seluruh body dengan
  `env->PushLocalFrame(16)` / `env->PopLocalFrame(nullptr)` (tiap early-return jadi
  return dari IIFE). Semua local transient (`bb/loaderCls/clCls/parent/loader/name` dan
  `jval/hooker/cb/sec/target/backup` + local yang dibuat ART/liblog di dalamnya) kini
  bebas di **setiap** jalur keluar (happy + tiap error), bukan hanya happy-path.
  GlobalRef (`g_cb_class`/`g_cb_object`/`g_backup`) & ref yang dipegang field statik
  (`spoof`/`original`) dibuat sebelum pop → selamat. **Nol perubahan perilaku**; ini
  kerapian, BUKAN fix leak — fungsi dipanggil sekali per-proses dan tabel local lama pun
  sudah auto-reclaimed saat `postAppSpecialize` return. `DeleteLocalRef(jval)` manual
  yang lama dihapus (kini ditangani frame).
- **`jni/main.cpp` — dokumentasi signal-set watchdog.** Tambah komentar di arm-site
  menjelaskan kenapa set `{ABRT,FPE,ILL}` sengaja TIDAK memuat SIGSEGV/SIGBUS (ART
  memakai SIGSEGV untuk implicit-null / stack-overflow / suspend / GC checks lalu pulih
  via handler-nya; arm SEGV = mencatat fault benign sebagai "CRASH"). **Komentar saja —
  biner tidak berubah.**

**Belum diverifikasi (gating — sama seperti seluruh L3):** cabang `SBX_ENABLE_LSPLANT=ON`
tak dapat di-compile di lingkungan review (lsplant/dobby/lsparself + `tt_hook_dex.h`
absent). Perubahan `tt_lsplant.hpp` di-review-by-spec (JNI PushLocalFrame/PopLocalFrame
+ LSPlant v6.4 API); **wajib** lolos compile + boot-test per-ABI saat flag di-enable.

**Tidak diubah (verdict false-positive/keep/defer):** `sandboxid.cpp`, `companion.cpp`,
`pool.hpp`, `config.hpp`, `SandboxIDHook.java`, `CMakeLists.txt`, `build.sh`.

---

## v1.0.27 (2026-08-18)

Rilis ini fokus ke **anti-leak identitas** (menutup nilai asli perangkat yang masih
bocor setelah persona Pixel dipasang) dan **anti-boot-loop** (mencegah persona yang
SDK-nya lebih tinggi dari OS asli). Semua temuan berasal dari trace alur boot → app
spawn → app membaca data sensitif. Istilah teknis dibiarkan Inggris.

### ⚠️ LEAK — SoC codename asli bocor lewat `ro.hardware` / `ro.board.platform`

- **Apa:** `ro.hardware` dan `ro.board.platform` kini di-drive dari persona (Tensor
  codename `gs101`/`gs201`/`zuma`/`zumapro`/`laguna`) secara end-to-end — field
  `platform` baru di `pool.hpp`, di-set oleh `gen_identity`, diserialisasi ke
  `identity.prop`, dipetakan di `hook_prop_get` (L2), lalu ditulis oleh `apply_native`
  (resetprop) **dan** `generate_mount_files` (build.prop sintetis).
- **Kenapa:** Versi sebelumnya sudah membuang hard-code `"qcom"`/`"sm8250"` dari tabel
  static default (benar — itu kontradiksi dengan Pixel/Tensor), TAPI tidak pernah
  memasang penggantinya: `gen_identity` tak men-set `BOARD_PLATFORM`, `serialize()`
  membuangnya, `apply_native`/build.prop tak menulis `ro.hardware`. Akibatnya kedua
  key jatuh ke chain `__system_property_get` → **SoC asli perangkat bocor** (mis.
  Snapdragon), padahal `Build.MODEL` mengklaim Pixel. Kombinasi Pixel-model +
  SoC-Qualcomm adalah sinyal deteksi yang kuat.
- **Mitigasi / catatan:** `gs101`/`gs201` sudah diverifikasi ke perangkat nyata.
  `zuma`/`zumapro`/`laguna` masih *widely-reported* dan **harus diverifikasi** ke unit
  asli / AOSP sebelum rilis luas — codename yang salah adalah tell baru (ditandai di
  komentar `pool.hpp`).

### ⚠️ LEAK — nama pasar asli bocor lewat `ro.product.marketname`

- **Apa:** Tambah pemetaan `ro.product.marketname` + `ro.product.vendor.marketname`
  → `MARKETNAME` di `hook_prop_get`, `MARKETNAME = model` di `gen_identity`, dan
  penulisannya di `apply_native` + build.prop.
- **Kenapa:** Key ini tak pernah dipetakan → app yang membaca `ro.product.marketname`
  mendapat nama pasar **perangkat asli** (mis. "Galaxy S23") sementara `Build.MODEL`
  bilang "Pixel 8". Inkonsistensi yang langsung terlihat.

### ⚠️ BOOT RISK — SDK gating (persona tak boleh lebih tinggi dari OS asli)

- **Apa:** `gen_identity` sekarang membaca SDK perangkat via `device_sdk()`
  (`ro.build.version.sdk`) lalu memfilter kandidat persona ke `sdk <= device_sdk`
  dan memilih acak dari yang lolos. Jika pembacaan SDK gagal, fallback ke persona
  ber-SDK **terendah** + warning (bukan ambil sembarang). Komentar `main.cpp` soal
  injeksi `SDK_INT` diperbaiki agar akurat ("does not exceed", bukan "equals").
- **Kenapa:** Sebelumnya persona dipilih acak dari seluruh pool tanpa melihat SDK
  asli. Jika `SDK_INT` persona **lebih tinggi** dari OS asli, app membaca
  `Build.VERSION.SDK_INT` lalu memanggil API framework yang belum ada di OS tersebut
  → `NoSuchMethodError` / crash / ANR.
- **Mitigasi:** Arah gating hanya turun (downgrade-only), yang memang arah aman;
  downgrade (persona ≤ device) tidak memicu API yang belum ada.

### ⚠️ LEAK — overlay mount dijamin tak merembes ke namespace zygote

- **Apa:** Setelah `setns` ke mount-namespace privat app, child companion kini
  menandai pohon namespace `MS_SLAVE | MS_REC` pada `"/"` sebelum melakukan bind.
  Bind-nya sendiri memang sudah dijalankan di `postAppSpecialize` (setelah app
  `unshare(CLONE_NEWNS)` di `SpecializeCommon`), bukan di `preAppSpecialize`.
- **Kenapa:** Jika bind terjadi sebelum app unshare — atau jika root namespace app
  masih `MS_SHARED` — overlay build.prop bisa **merembes ke namespace zygote** dan
  otomatis nge-spoof **setiap app yang lahir sesudahnya** (global leak, bukan per-app).
  `MS_SLAVE` memutus propagasi keluar sebagai belt-and-suspenders di atas timing
  `postAppSpecialize` yang sudah benar.
- **Mitigasi:** Best-effort — kegagalan `MS_SLAVE` bersifat non-fatal karena timing
  post-specialize sudah menjaga isolasi utama.

### Changed — RADIO (baseband) stabil per-persona

- **Apa:** `RADIO` / `gsm.version.baseband` kini diturunkan stabil dari persona:
  `modem_prefix(platform)` (mis. `gs101`→`g5123b`) + tanggal dari `security_patch`
  + potongan `incremental` — bukan lagi tanggal hari-ini + string modem hard-code.
- **Kenapa:** Versi lama meregenerasi RADIO dengan tanggal `localtime` **setiap
  `freshen`** → baseband berubah tiap rotasi dan tidak match platform persona.
  Baseband yang berubah tanpa OTA adalah anomali; sekarang tetap sama untuk persona
  yang sama.
- **Catatan:** Nilai `modem_prefix` bersifat *best-effort* dan sebaiknya diverifikasi
  ke firmware Pixel asli.

### Changed — hapus "detection tell" di build.prop sintetis

- **Apa:** Komentar header `# SandboxID synthetic build.prop (v1.0.3)` dan
  `# Partition alias` dihapus dari file mount, diganti header standar
  `# begin build properties`.
- **Kenapa:** App yang membaca `/system/build.prop` bisa `grep` string "SandboxID" /
  "synthetic" → fingerprint modul yang trivial. build.prop asli tak pernah memuat nama
  modul.

### Changed — companion fork-safety + diagnostik mount

- **Apa:** Child hasil `fork()` di companion tidak lagi memanggil `__android_log_print`
  sama sekali; ia mengumpulkan hasil ke POD `MountResult` (ok/fail/skip + errno per
  mount) dan mengirimnya ke **thread parent** via pipe, yang kemudian melakukan
  logging. VLA `src_fds` diganti `std::array<int, BIND_ENTRIES_N>`.
- **Kenapa:** Companion multi-thread (satu thread per `connectCompanion`). Memanggil
  liblog di child setelah `fork()` berisiko **deadlock** karena mutex liblog dapat
  terwarisi dalam keadaan terkunci (fork tak menyalin thread pemegang lock). VLA
  berukuran non-konstan juga non-standar padahal jumlah bind entry compile-time
  constant.
- **Bonus:** errno per-mount (`EPERM`=SELinux/caps, `EINVAL`=flags, `ENOENT`=target
  tak ada) kini di-log parent untuk memudahkan diagnosa bind gagal.

### Changed — single source of truth (`config.hpp`)

- **Apa:** Enum `Cmd`, path, tabel `BindEntry`, dan fallback properti dipindah ke
  `config.hpp`; duplikatnya di `main.cpp`/`companion.cpp` dihapus. Ditambah path
  `IDENTITY_BAK`/`MODE_FILE`/`RESETPROP` dan array `MOUNT_PARTS[]` yang dipakai CLI.
- **Kenapa:** Sebelumnya `main.cpp` mendaftar 6 bind entry sementara `companion.cpp`
  9 — **drift** yang membuat overlay tidak konsisten. Satu sumber menghapus kelas bug
  ini dan membuat data auditable di satu tempat.

### Fixed — identity.prop kosong saat spawn (race dengan `seed`)

- **Apa:** `CMD_GET_IDENTITY` di companion kini me-retry baca `identity.prop` 3×100ms
  bila kosong (file sedang mid-replace / seed telat), lalu `LOGE` pesan actionable
  ("run `sandboxid seed`/`freshen`"). Balasan kosong tetap fail-safe (app dianggap
  non-target, tidak crash).
- **Kenapa:** `identity.prop` seharusnya sudah ada sebelum zygote (post-fs-data
  `seed`), tapi jika seed gagal/telat app menerima blob kosong tanpa jejak. Retry +
  LOGE menutup jendela race kecil dan membuat kegagalan terlihat di logcat.

---

## v1.0.19 (2026-07-27)

### Action button is now 1-tap ready
- `action.sh` now runs **`bin/sandboxid freshen` → `rotate_ids.sh all`** in sequence.
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
1. `bin/sandboxid freshen` — rolls `MODEL`, `DEVICE`, `BRAND`, `SERIAL`, `ANDROID_ID`, `GOOGLE_AID`, etc.
2. `wipe_ssaid` — deletes `settings_ssaid.xml` per user (needs reboot to regenerate).
3. `set_gaid_value` — syncs `GOOGLE_AID` to `Settings.Global.advertising_id` + GMS `adid_settings.xml`.
4. `randomize_wlan_mac` — wlan0 MAC + wipes `WifiConfigStore.xml`.
5. `rotate_bluetooth_mac` — BT adapter MAC + `bt_config.conf` Address.
6. `sync_device_name` — device_name/bluetooth_name/`bt_config.conf` Name = `identity.prop` MODEL.

---

# Changelog

All notable changes to SandboxID are recorded here. The GitHub Actions workflow
reads the matching `## vX.Y.Z` section to build `release_notes.md` automatically
on every release.

Format loosely follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and [Semantic Versioning](https://semver.org/).

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

- Migrated repository from `diru768/sandboxid` to `Ilham311/sandboxid`. All README badges, install links, `git clone` URL, `module.prop` `updateJson`, and `update.json` seed now point to the new repo.
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

- **Runtime whitelist `target.txt`** at `/data/adb/modules/sandboxid/target.txt`. Add / remove target packages without rebuilding.
- Companion loads and **hot-reloads** `target.txt` on mtime change (next app spawn picks up edits).
- New CLI subcommand `sandboxid targets` to dump the active whitelist.
- L7 `SUPPRESS` label for known log-noise keys (`log.looper.*.slow`, `debug.watson.*`) to keep summaries readable.
- `summarize.sh` now breaks SPOOF hits out by hook layer (`L1` / `L2` / `L7-SPB` / `L7-SPI` / `L7-SPL`) and counts `SUPPRESS`, `REJECT`, `ACCEPT` separately.
- `customize.sh` **preserves** existing `target.txt` across reinstalls.

### Changed

- Zygisk companion IPC protocol for `CMD_GET_IDENTITY` now includes the pkg name; companion responds with `len=0` for non-targets (single source of truth for whitelist).
- Zygisk `.so` no longer contains a hardcoded target list.
- `sandboxid.cpp` `wipe_target_data()` reads targets from `target.txt` for symmetry with the Zygisk side.

### Fixed

- Whitelist drift between the CLI (`sandboxid`) and Zygisk companion — both now share one file.

---

## v1.0.14

### Added

- `post-fs-data.sh` + new `sandboxid seed` subcommand that generates identity + mount overlay files **before** Zygisk loads, fixing the first-boot race where the first TT/Grab pid got 0/6 bind mounts.
- Android 11+ canonical partition paths in `BIND_ENTRIES` (`/odm/etc/build.prop`, `/product/etc/build.prop`, `/system_ext/etc/build.prop`) alongside the legacy paths.
- Skip counter is split into `skip_src` (module bug) vs `skip_dst` (device doesn't have that partition — expected).

### Fixed

- 3-skip on POCO F3 / MIUI-style ROMs where partition build.prop lives at `/{partition}/etc/build.prop`.

---

## v1.0.13

### Added

- **Per-type L7 spoof maps** (`sbx_bool_spoof`, `sbx_int_spoof`, `sbx_long_spoof`) consulted by the typed `native_get_*` hooks before falling back to `def`.
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
- Action tap on debug variant now auto-produces `summary-YYYYMMDD-HHMMSS.txt`, copies `crashes.log`, and gzips the raw log to `/sdcard/Download/sandboxid-logs/`.
- Automatic pruning: keeps newest 10 summaries / crashes / raw.gz per install.

---

## v1.0.10

### Added

- **Zero-setup auto-log capture** on debug variant. `service.sh` starts a background logcat on boot into `/data/adb/modules/sandboxid/debug/session-YYYYMMDD-HHMMSS.log`, keeping the 5 newest sessions.
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
- `SBX_DEBUG` compile-time flag: release strips `LOGD` calls entirely (zero cost), debug keeps them.

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
- `sandboxid freshen` regenerates all overlay files.

---

## v1.0.2

### Added

- `resetprop-rs` invocation from `apply-boot` and `freshen` to broadcast native property changes.

---

## v1.0.1

### Added

- Standalone CLI (`sandboxid`) with `freshen`, `status`, `rollback`, `lock`, `unlock`, `apply-boot` subcommands.

---

## v1.0.0

Initial release.

- 6-layer Java hook: `Build.*`, `SystemProperties.native_get`, `Settings.Secure.getString`, `AdvertisingIdClient.Info.getId` (stub), `WifiInfo.getMacAddress` / `getBSSID`, `TelephonyManager.getImei` / `getDeviceId` / `getSubscriberId` / `getMeid`.
- Pixel-only device pool (SDK 33–36).
- TikTok Global / Asia / Lite target packages.
