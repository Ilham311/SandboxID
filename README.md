<div align="center">

# Ternak TT

**Zygisk fresh-persona module untuk TikTok & Grab**

[![Build & Release](https://github.com/diru768/ternak-tt/actions/workflows/build.yml/badge.svg)](https://github.com/diru768/ternak-tt/actions/workflows/build.yml)
[![Latest Release](https://img.shields.io/github/v/release/diru768/ternak-tt?label=release&color=blue)](https://github.com/diru768/ternak-tt/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-green)](./LICENSE)
![Android](https://img.shields.io/badge/android-13%2B-brightgreen)
![Zygisk](https://img.shields.io/badge/zygisk-ZygiskNext%20%7C%20HMA--OSS%20%7C%20ReZygisk-orange)

Rotate device identity, wipe app data, and hide the real fingerprint from TikTok (musically / trill / lite) and Grab in one tap.

</div>

---

## Contents

- [Highlights](#highlights)
- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Install](#install)
- [Usage](#usage)
- [Runtime whitelist (`target.txt`)](#runtime-whitelist-targettxt)
- [Debug variant](#debug-variant)
- [Build from source](#build-from-source)
- [Auto-release pipeline](#auto-release-pipeline)
- [Scope](#scope)
- [Roadmap](#roadmap)
- [License](#license)

---

## Highlights

- **7 hook layers** cover Java `Build.*`, `SystemProperties.native_get` (all typed variants), `Settings.Secure`, GAID stub, WiFi MAC/BSSID, and Telephony IMEI/subscriber ID.
- **Companion fork + `setns` bind-mount overlay** rewrites `/system/build.prop`, `/vendor/build.prop`, `/odm/etc/build.prop`, `/product/etc/build.prop`, `/system_ext/etc/build.prop`, and `settings_secure.xml` inside the target's mount namespace — invisible to other apps.
- **Post-fs-data early seed** generates identity + overlay files before Zygote starts, so the first TT/Grab spawn already gets 6/6 bind mounts (no first-boot race).
- **Runtime whitelist** (`target.txt`) — add or remove targets by editing one file, no rebuild. Companion hot-reloads on mtime change.
- **Crash watchdog** catches `SIGABRT` / `SIGFPE` / `SIGILL` / `SIGSYS`, rate-limited to 3 per signal, and restores `SIG_DFL` so ART's tombstone flow still fires.
- **Debug variant** auto-captures per-boot `session-<ts>.log`, produces a small chat-shareable summary on Action tap, and keeps a persistent `crashes.log` journal.

---

## How it works

```
┌─ Boot ────────────────────────────────────────────────────────────┐
│  post-fs-data.sh  ->  ternak-tt seed                              │
│     └ generates identity.prop + mount/*/build.prop + xml          │
│                                                                   │
│  service.sh (after sys.boot_completed=1)                          │
│     └ ternak-tt apply-boot  (resetprop native broadcast)          │
└───────────────────────────────────────────────────────────────────┘

┌─ Per app spawn ─────────────────────────────────────────────────────┐
│  Zygisk .so  ──[CMD_GET_IDENTITY + pkg name]──>  companion         │
│                                                     │              │
│                                                     v              │
│                                     read target.txt (cached, mtime)│
│  Zygisk .so  <────[blob or len=0]───  companion                    │
│     └ if target: install 7 hook layers + request bind-mount        │
│     └ companion forks child -> setns(target mnt ns) -> mount(BIND) │
└────────────────────────────────────────────────────────────────────┘
```

---

## Requirements

| Item | Version |
|------|---------|
| Android | 13+ (API 33+) |
| Root manager | KernelSU / Magisk ≥ 26100 / APatch |
| Zygisk provider | ZygiskNext, HMA-OSS, or ReZygisk (Magisk built-in also works) |
| ABI | arm64-v8a, armeabi-v7a, x86_64, x86 |

---

## Install

1. Grab the latest release: **[Releases → latest](https://github.com/diru768/ternak-tt/releases/latest)**
2. Pick a variant:
   - `ternak-tt-vX.Y.Z-release.zip` — production build, stripped, `LOGD` compiled out
   - `ternak-tt-vX.Y.Z-debug.zip` — verbose logs auto-captured to `/data/adb/modules/ternak_tt/debug/`
3. Flash via KernelSU / Magisk manager → **Modules** → **Install from storage**
4. **Reboot**
5. Tap the module's **Action** button in your root manager to rotate persona

---

## Usage

All commands run under root:

```bash
su -c 'ternak-tt <command>'
```

| Command | What it does |
|---------|--------------|
| `freshen` | Rotate persona (new identity + regenerate mount files + wipe target app data) |
| `status` | Print current `identity.prop` |
| `rollback` | Restore previous identity from backup |
| `lock` / `unlock` | Prevent / re-enable `freshen` (safety after account login) |
| `apply-boot` | Re-apply native props via `resetprop-rs` (used by `service.sh`) |
| `seed` | Fast bootstrap: identity + mount overlay only (used by `post-fs-data.sh`) |
| `targets` | List active whitelist from `target.txt` |

Most users only need `freshen` — which is exactly what the **Action** button runs.

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

## Debug variant

The `-debug.zip` build enables:

- Verbose `[D]` traces from `TernakTT` and `TernakTTCompanion` in `logcat`
- Automatic per-session logging to `/data/adb/modules/ternak_tt/debug/session-<ts>.log`
- Persistent crash journal at `debug/crashes.log`
- **Action tap** produces:
  - `summary-<ts>.txt` (compact digest, safe to paste in chat)
  - `session-<ts>.log.gz` (full log)
  - copied to `/sdcard/Download/ternak-tt-logs/`

Live logcat:

```bash
su -c 'logcat -c && logcat -v time -s TernakTT:V TernakTTCompanion:V'
```

---

## Build from source

```bash
git clone https://github.com/diru768/ternak-tt.git
cd ternak-tt

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
- `ANDROID_ID` spoof (v1.0; full Java hook via lsplant in v1.1)
- WiFi MAC / BSSID zeroing
- Telephony `IMEI` / `getDeviceId` / `getSubscriberId` / `getMeid` null-ing
- Bind-mount overlay for 5 `build.prop` files + `settings_secure.xml`
- Automatic target app-data wipe on `freshen`

### Out of scope

- X-Ladon / X-Gorgon / X-Argus / `ttreq` request signatures
- TLS / JA3 fingerprint
- Behavior automation
- Network layer (VPN, residential IP)
- Play Integrity / SafetyNet (use `tricky_store` + `zygisk_shamiko` alongside)
- Sensor fingerprint (v1.1)

---

## Roadmap

| Version | Focus |
|---------|-------|
| **v1.1** | Bundle `lsplant` for real Java hook (L3 Settings.Secure, L4 GAID), sensor spoof, per-library PLT hook |
| **v1.2** | Per-account snapshot, auto-rotate scheduler, WebUI |
| **v2.0** | Douyin support, Shelter/Island integration |

---

## License

[MIT](./LICENSE)

---

## Disclaimer

For **security research and educational purposes**. Any use that violates a platform's Terms of Service is the sole responsibility of the user.
