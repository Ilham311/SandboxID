<div align="center">

# Ternak TT

**Zygisk fresh-persona module untuk TikTok & Grab**

[![Build & Release](https://github.com/Ilham311/Tt/actions/workflows/build.yml/badge.svg)](https://github.com/Ilham311/Tt/actions/workflows/build.yml)
[![Latest Release](https://img.shields.io/github/v/release/Ilham311/Tt?label=release&color=blue)](https://github.com/Ilham311/Tt/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-green)](./LICENSE)
![Android](https://img.shields.io/badge/android-13%2B-brightgreen)
![Zygisk](https://img.shields.io/badge/zygisk-ZygiskNext%20%7C%20HMA--OSS%20%7C%20ReZygisk-orange)

Spoofs the device identity that apps see: model, brand, manufacturer, build fingerprint, serial, per-app Android ID / SSAID, GAID, wlan/Bluetooth MAC, and device/BT name — all in one tap.

</div>

---

## What it does

Ternak TT rotates the identity strings apps read at runtime. Property spoofing happens **pre-zygote**, before apps launch, so the first spawn of TikTok / Grab / any package in `target.txt` already sees the new persona. It only changes safe identity strings — it avoids risky hardware or framework changes that can break boot.

**One tap = full rotation:**
1. `bin/ternak-tt freshen` writes a new `identity.prop` (MODEL, DEVICE, BRAND, MANUFACTURER, FINGERPRINT, SERIAL, ANDROID_ID, GOOGLE_AID, ...).
2. `rotate_ids.sh all` syncs the shell layer: SSAID wipe, GAID, wlan MAC, Bluetooth MAC, device/BT name — all reading from the same `identity.prop` so hook layer and shell layer report **identical** values.

---

## Contents

- [Highlights](#highlights)
- [Architecture](#architecture)
- [Requirements](#requirements)
- [Install](#install)
- [Usage](#usage)
- [`rotate_ids.sh` CLI](#rotate_idssh-cli)
- [Runtime whitelist (`target.txt`)](#runtime-whitelist-targettxt)
- [`identity.prop` schema](#identityprop-schema)
- [Debug variant](#debug-variant)
- [Build from source](#build-from-source)
- [Auto-release pipeline](#auto-release-pipeline)
- [Scope](#scope)
- [Roadmap](#roadmap)
- [References & further reading](#references--further-reading)
- [License](#license)

---

## Highlights

- **1-tap Action** — `action.sh` runs `freshen` then `rotate_ids.sh all` so the KSU / Magisk manager Action button applies a complete rotation end-to-end. No manual `sh rotate_ids.sh` needed.
- **Persona-consistent shell layer** — `sync_device_name`, `set_gaid_value`, `rotate_bluetooth_mac` all read from `identity.prop`, so the L1/L2 Java hooks and shell layer never disagree (fixes the `randomize_device_name` clash from earlier versions that picked a random `Galaxy S24-427` on top of a `Pixel 8` hook).
- **2 active hook layers** cover Java `Build.*` and `SystemProperties.native_get` (all typed variants).
- **Companion fork + `setns` bind-mount overlay** rewrites `/system/build.prop`, `/vendor/build.prop`, `/odm/etc/build.prop`, `/product/etc/build.prop`, `/system_ext/etc/build.prop`, and `settings_secure.xml` inside the target's mount namespace — invisible to other apps.
- **Post-fs-data early seed** generates identity + overlay files before Zygote starts, so the first TT/Grab spawn already gets 6/6 bind mounts (no first-boot race).
- **Runtime whitelist** (`target.txt`) — add/remove targets by editing one file, no rebuild. Companion hot-reloads on mtime change.
- **Bluetooth MAC rotation** — writes 5 BT MAC props + rewrites `Address =` in `bt_config.conf` (3 paths), persisted to `identity.prop` as `BLUETOOTH_ADDR`.
- **wlan MAC + WifiConfigStore backup** — backs up `WifiConfigStore.xml` before deleting, no silent wifi loss.
- **SSAID surgical wipe** — per-user `settings_ssaid.xml` backed up then removed; system_server regenerates a clean SSAID at next boot.
- **Auto-rotating backups** — every destructive op copies the target file into `$MODDIR/backups/` first (`backup_rotate` keeps 10).
- **Reboot-idempotent identity** — `WIFI_MAC`, `BLUETOOTH_ADDR`, `BLUETOOTH_NAME` persisted to `identity.prop` so the next tap keeps the same values until an explicit re-rotate.
- **Crash watchdog** catches `SIGABRT` / `SIGFPE` / `SIGILL` / `SIGSYS`, rate-limited to 3 per signal, and restores `SIG_DFL` so ART's tombstone flow still fires.
- **Debug variant** auto-captures per-boot session logs (`session-YYYYMMDD-HHMMSS.log`), produces a chat-shareable summary on Action tap, keeps a persistent `crashes.log` journal.

---

## Architecture

```
┌─ Boot ──────────────────────────────────────────────┐
│  post-fs-data.sh  ->  ternak-tt seed                              │
│     └ generates identity.prop + mount/*/build.prop + xml          │
│                                                                   │
│  service.sh (after sys.boot_completed=1)                          │
│     └ ternak-tt apply-boot  (resetprop native broadcast)          │
└─────────────────────────────────────────────────────┘

┌─ Action button tap (KSU / Magisk manager) ─────────────────────┐
│  action.sh                                                        │
│    Step 1:  bin/ternak-tt freshen                                 │
│              └ new identity.prop  (MODEL, DEVICE, ANDROID_ID, ...) │
│              └ settings put global device_name MODEL              │
│    Step 2:  sh rotate_ids.sh all                                  │
│              └ wipe_ssaid           (backup /data/system/users/*)  │
│              └ set_gaid_value       (Settings.Global + GMS xml)    │
│              └ randomize_wlan_mac   (ip link + WifiConfigStore)    │
│              └ rotate_bluetooth_mac (5 props + bt_config Address)  │
│              └ sync_device_name     (reads identity.prop MODEL)    │
└──────────────────────────────────────────────────────────┘

┌─ Per app spawn ─────────────────────────────────────────────┐
│  Zygisk .so  ──[CMD_GET_IDENTITY + pkg name]──>  companion         │
│                                                     │              │
│                                                     v              │
│                                     read target.txt (cached, mtime)│
│  Zygisk .so  <────[blob or len=0]───  companion                    │
│     └ if target: install 2 active hook layers + request bind-mount │
│     └ companion forks child -> setns(target mnt ns) -> mount(BIND) │
└─────────────────────────────────────────────────────┘
```

---

## Requirements

| Item | Version |
|------|---------|
| Android | 13+ (API 33+) |
| Root manager | KernelSU / Magisk >= 26100 / APatch |
| Zygisk provider | ZygiskNext, HMA-OSS, ReZygisk, or NeoZygisk (Magisk built-in also works) |
| ABI | arm64-v8a, armeabi-v7a, x86_64, x86 |

---

## Install

1. Grab the latest release: **[Releases -> latest](https://github.com/Ilham311/Tt/releases/latest)**
2. Pick a variant:
   - `ternak-tt-vX.Y.Z-release.zip` — production build, stripped, `LOGD` compiled out
   - `ternak-tt-vX.Y.Z-debug.zip` — verbose logs auto-captured to `/data/adb/modules/ternak_tt/debug/`
3. Flash via KernelSU / Magisk manager -> **Modules** -> **Install from storage**
4. **Reboot**
5. Tap the module's **Action** button in your root manager — that single tap runs `freshen` + `rotate_ids.sh all`

---

## Usage

Most users only need the **Action** button. For manual control:

### `ternak-tt` CLI (native)

```bash
su -c 'ternak-tt <command>'
```

| Command | What it does |
|---------|--------------|
| `freshen` | Rotate persona (new identity + regenerate mount files) |
| `status` | Print current `identity.prop` |
| `rollback` | Restore previous identity from backup |
| `lock` / `unlock` | Prevent / re-enable `freshen` (safety after account login) |
| `apply-boot` | Re-apply native props via `resetprop-rs` (used by `service.sh`) |
| `seed` | Fast bootstrap: identity + mount overlay only (used by `post-fs-data.sh`) |
| `targets` | List active whitelist from `target.txt` |

### `rotate_ids.sh` CLI

Shell-layer rotation using values from `identity.prop`.

```bash
su -c 'sh /data/adb/modules/ternak_tt/rotate_ids.sh <cmd>'
```

| Command | Applies | Needs reboot? |
|---------|---------|---------------|
| `all` | SSAID + GAID + wlan MAC + BT MAC + device name (default) | Yes (SSAID) |
| `safe` | GAID + BT MAC + device name (skips SSAID + wlan) | No |
| `ssaid` | Delete `settings_ssaid.xml` per user | Yes |
| `gaid [uuid]` | Set Google Advertising ID | No |
| `wlan-mac [xx:xx:...]` | Set `wlan0` MAC + wipe WifiConfigStore | No |
| `bt-mac [xx:xx:...]` | Set Bluetooth adapter MAC + `bt_config.conf` Address | No (toggle BT) |
| `device-name [name]` | Sync device/BT name to `identity.prop` MODEL | No |
| `status` | Read-only snapshot of all identifiers | — |
| `help` | Print usage | — |

---

## Runtime whitelist (`target.txt`)

The list of packages hooked by Zygisk lives at:

```
/data/adb/modules/ternak_tt/target.txt
```

**Format:** one package per line. Blank lines and `#` comments are ignored.

**Defaults shipped:**

```
com.zhiliaoapp.musically
com.ss.android.ugc.trill
com.zhiliaoapp.musically.go
com.grabtaxi.passenger
```

Edit the file, save, and the companion re-reads it on the next app spawn (mtime watch). Verify with:

```bash
su -c 'ternak-tt targets'
```

Your customized `target.txt` is **preserved across reinstalls** by `customize.sh`.

---

## `identity.prop` schema

Written by `freshen`, read by native prop apply, Zygisk hooks, and `rotate_ids.sh`. Located at `/data/adb/modules/ternak_tt/identity.prop`.

| Key | Written by | Purpose |
|-----|-----------|---------|
| `MODEL` | `freshen` | `Build.MODEL`, `ro.product.model` (all partitions) |
| `BRAND` | `freshen` | `Build.BRAND`, `ro.product.brand` |
| `MANUFACTURER` | `freshen` | `Build.MANUFACTURER`, `ro.product.manufacturer` |
| `DEVICE` | `freshen` | `Build.DEVICE`, `ro.product.device`, `ro.build.product` |
| `PRODUCT` | `freshen` | `Build.PRODUCT`, `ro.product.name` |
| `BOARD`, `HARDWARE` | `freshen` | `ro.product.board`, `ro.hardware` |
| `FINGERPRINT`, `ID`, `DISPLAY` | `freshen` | Build metadata |
| `SERIAL` | `freshen` | `Build.SERIAL`, `ro.serialno`, `ro.boot.serialno` |
| `RADIO` | `freshen` | `Build.RADIO`, `gsm.version.baseband` |
| `ANDROID_ID` | `freshen` | Per-app `Settings.Secure.ANDROID_ID` (L1/L2 hook) |
| `GOOGLE_AID` | `freshen` | GAID (also written by `set_gaid_value` if missing) |
| `WIFI_MAC` | `rotate_ids.sh` | Persisted wlan0 MAC (v1.0.19+) |
| `BLUETOOTH_ADDR` | `rotate_ids.sh` | Persisted BT adapter MAC (v1.0.19+) |
| `BLUETOOTH_NAME` | `rotate_ids.sh` | Optional override for device/BT name (v1.0.19+); if unset, uses `MODEL` |

Use `identity_get KEY` / `identity_persist KEY VALUE` from `helpers.sh` for programmatic access. Atomic upsert via awk + rename.

---

## Debug variant

The `-debug.zip` build enables:

- Verbose `[D]` traces from `TernakTT` and `TernakTTCompanion` in `logcat`
- Automatic per-session logging to `/data/adb/modules/ternak_tt/debug/`
- Persistent crash journal at `debug/crashes.log`
- **Action tap** produces:
  - `summary-YYYYMMDD-HHMMSS.txt` — compact digest, safe to paste in chat
  - `session-YYYYMMDD-HHMMSS.log.gz` — full log
  - copied to `/sdcard/Download/ternak-tt-logs/`

Live logcat:

```bash
su -c 'logcat -c && logcat -v time -s TernakTT:V TernakTTCompanion:V'
```

---

## Build from source

```bash
git clone https://github.com/Ilham311/Tt.git
cd Tt

export ANDROID_NDK_HOME=/opt/android-ndk-r26d

cp /path/to/resetprop prebuilt/resetprop-rs   # optional but recommended

curl -fsSL -o jni/zygisk.hpp \
  https://raw.githubusercontent.com/topjohnwu/zygisk-module-sample/master/module/jni/zygisk.hpp

./build.sh    # produces dist/ternak-tt-<version>-{release,debug}.zip
```

Env overrides:

| Var | Default | Meaning |
|-----|---------|---------|
| `VARIANT` | `both` | `release`, `debug`, or `both` |
| `MIN_SDK` | `33` | Android platform target |

---

## Auto-release pipeline

Everything is driven by `.github/workflows/build.yml`:

1. Reads `module.prop` (single source of truth for version/code)
2. Auto-bumps patch if the tag already exists
3. Builds both variants across 4 ABIs
4. Computes SHA-256 checksums
5. **Auto-generates `release_notes.md`** from the matching section in `CHANGELOG.md` plus git log since the previous tag
6. **Auto-generates `update.json`** from `module.prop` and the current repo slug
7. Commits refreshed `update.json` + synced `module.prop` back to `main`
8. Creates a GitHub Release with both zips, `checksums.sha256`, and `update.json` attached

No manual input needed — push a commit to `main`, get a release.

---

## Scope

### Covered

- Device fingerprint (`Build.*`, all `SystemProperties.native_get*` typed variants)
- Per-app `ANDROID_ID` / SSAID spoof (Java hook + system XML wipe)
- Google Advertising ID (`Settings.Global.advertising_id` + GMS `adid_settings.xml`)
- WiFi MAC + `WifiConfigStore.xml` reset (with backup)
- Bluetooth adapter MAC (5 props + `bt_config.conf` Address across 3 paths)
- Device / Bluetooth name synced to persona MODEL
- Telephony `IMEI` / `getDeviceId` / `getSubscriberId` / `getMeid` null-ing
- Bind-mount overlay for 5 `build.prop` files + `settings_secure.xml`

### Out of scope

- X-Ladon / X-Gorgon / X-Argus / `ttreq` request signatures
- TLS / JA3 fingerprint
- Behavior automation
- Network layer (VPN, residential IP)
- Play Integrity / SafetyNet (use `PlayIntegrityFork` + `Shamiko` + `TrickyStore` alongside — see [References](#references--further-reading))
- Sensor fingerprint (planned)
- **MediaDrm ID** — modern anchor identifier, harder to spoof (planned, see [References](#references--further-reading))
- **GSF ID** — `com.google.android.gsf/databases/gservices.db`, needs sqlite3 on-device (planned)

---

## Roadmap

| Version | Focus |
|---------|-------|
| **v1.0.20** | GSF ID rotation, dlopen-based linker path for Zygisk Next detection resistance |
| **v1.1** | Bundle [LSPlant](https://github.com/LSPosed/LSPlant) for true Java hook (upgrade L1/L2 from `native_get` to method-level), per-account snapshot store |
| **v1.2** | Sensor fingerprint spoof (accelerometer/gyro noise, sensor list mask) |
| **v1.3** | MediaDrm ID rotation (widevine L3 CDM ID), App Set ID native hook |
| **v2.0** | WebUI (KSU/APatch WebUI style, see [COPG](https://github.com/AlirezaParsi/COPG) reference), Douyin support, per-account auto-rotate scheduler |

---

## References & further reading

### Hook frameworks & Zygisk providers

- **[LSPlant](https://github.com/LSPosed/LSPlant)** — Java method hook framework for ART, supports Android 5.0-17 (API 21-37). Planned upgrade for L1/L2 to move from `SystemProperties.native_get` intercept to true `Build.getSerial()` / `Settings.Secure.getString()` method-level hook.
- **[ReZygisk](https://github.com/PerformanC/ReZygisk)** — fork of Zygisk Next, rewritten in C with custom linker to defeat linker-based Zygisk detection. Recommended Zygisk provider for KernelSU + SUSFS setups.
- **[NeoZygisk](https://github.com/JingMatrix/NeoZygisk)** — modern Zygisk implementation, active alternative to ReZygisk.
- **[Zygisk Next](https://github.com/Dr-TSNG/ZygiskNext)** — standalone Zygisk for KernelSU, still widely deployed.
- **[HMA-OSS](https://github.com/frknkrc44/HMA-OSS)** — fork of Hide My Applist, hides package list from TT/Grab's `getInstalledPackages()` scan. Complementary to Ternak TT.
- **[Zygisk Assistant](https://github.com/snake-4/Zygisk-Assistant)** — FOSS root hider, Shamiko alternative.

### Reference spoofers (design inspiration)

- **[COPG](https://github.com/AlirezaParsi/COPG)** by AlirezaParsi — per-app device/CPU/GPU spoofer with full on-device WebUI. Reference for the v2.0 WebUI roadmap.
- **[DeviceID/SSAID Changer](https://github.com/sidex15/deviceidchanger)** by sidex15 — WebUI module for SSAID rotation; validates the SSAID-wipe + reboot approach.
- **[DeviceSpoofLab-Magisk](https://github.com/yubunus/DeviceSpoofLab-Magisk)** by yubunus — simple identifier reset module, useful as a minimal reference.
- **[Treat Wheel](https://t.me/yuriiroot)** by Yuri — property spoof system with resetprop-less path ("fixing dirty serials without resetprop"). Design hint for the `resetprop-rs`-optional path in `helpers.sh`.

### Play Integrity companions

- **[Play Integrity Fork](https://github.com/osm0sis/PlayIntegrityFork)** by osm0sis — fingerprint-props spoof + Tricky Store integration for A13+ DEVICE/STRONG integrity. Ternak TT and PIFork can coexist; disable PIFork's `ro.product.*` spoofing to avoid clash.
- **[TrickyStore](https://github.com/5ec1cff/TrickyStore)** — modifies the certificate chain for Android key attestation. Required for STRONG integrity on <A13.
- **[Shamiko](https://github.com/LSPosed/LSPosed.github.io/releases)** — Zygisk-based root hider with denylist enforcement.
- **[Universal Play Integrity guide (XDA)](https://xdaforums.com/t/guide-how-to-pass-strong-integrity-on-android-step-by-step-guide.4729435/)** — step-by-step for BASIC + DEVICE + STRONG on both legacy and new verdicts.

### Detection & background

- **[From IMEI to MediaDrm: The Evolution and Breakdown of Android Device Identity](https://medium.com/@identx_labs/from-imei-to-mediadrm-id-the-evolution-and-breakdown-of-android-device-identity-9f14d49c6d98)** by IdentX Labs — modern anchor identifiers (Android ID, GSF ID, MediaDrm ID) and why they persist. Roadmap justification for v1.3 MediaDrm work.
- **[Best practices for unique identifiers](https://developer.android.com/identity/user-data-ids)** by Android Developers — official guidance on identifier scoping.
- **[Device Intelligence Spoofing Techniques](https://www.incognia.com/blog/device-intelligence-spoofing)** by Incognia — fraud-detection perspective, useful for anticipating what target apps check.
- **[How Attackers Bypass Play Integrity in the Wild](https://medium.com/@vaibhav.shakya786/how-attackers-bypass-play-integrity-api-in-the-wild-f1091aea36e9)** — lifecycle assumption failures, token replay windows.
- **[Android app fingerprinting](https://discuss.privacyguides.net/t/android-app-fingerprinting/27641)** (Privacy Guides discussion) — covers `MediaStore` fingerprinting lockdown in Android 16 (API 36) and adjacent surfaces.

---

## License

[MIT](./LICENSE) (c) 2026 Ilham311

---

## Disclaimer

For **security research and educational purposes**. Any use that violates a platform's Terms of Service is the sole responsibility of the user.
