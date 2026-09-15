# SandboxID

**Android device identifier privacy research & education module**

SandboxID is an open-source Zygisk module for studying and experimenting with
the device identifier fields that applications read on a device you own. It
exposes Android `Build.*` properties and per-application identity configuration
through a userland module plus a CLI/shell layer, so you can observe how apps
behave against varied device configurations and learn Android internals.

- Neutral and universal — no opinion about which applications to touch.
- User data sovereignty — you decide every configuration; the module ships idle.
- Transparent — same mechanism the platform uses (`Build.*`, Android property
  system), surfaced for learning rather than hidden.

---

## Introduction

SandboxID is intended for:

- **Privacy researchers** — inspect which identifier fields apps read and how
  they are derived from the Android property system.
- **Security educators** — demonstrate the `Build.*` / `SystemProperties`
  mechanism and per-application configuration in a controlled, local setting.
- **Mobile developers** — validate application behavior against varied device
  configurations without needing many physical devices.
- **Android internals learners** — explore how pre-zygote property setup and
  per-application configuration work on a real device.

Philosophy:

- **User data sovereignty** — the device owner holds full control over which
  identifiers are visible on their own device.
- **Transparency** — behavior is documented and auditable; no opaque defaults.
- **Education** — the module is a teaching surface for Android identifier
  mechanics, not a product with a fixed target.

All processing happens on the device owner's hardware. The module does nothing
to any application until you configure `target.txt`.

---

## How it works

SandboxID combines three cooperating layers:

1. **Pre-zygote property configuration** — a Zygisk module runs before
   application processes start. It reads a per-package identity blob from the
   companion and patches the Java `Build.*` fields and `SystemProperties`
   reads the app would otherwise observe, so the first spawn already sees the
   configured values.
2. **Per-application identifier customization** — a root-side companion process
   serves identity blobs over the Zygisk socket, hot-reloads the target list,
   and bind-mounts a synthetic `build.prop` tree into the target's mount
   namespace so file-based readers see consistent values.
3. **CLI / shell layer** — a native `sandboxid` binary and `rotate_ids.sh`
   script regenerate the persona, apply native properties, and synchronize
   shell-layer identifiers (SSAID, GAID, wlan/Bluetooth MAC, device name).

The architecture is deliberately split so the Zygisk hook layer and the shell
layer report the same values for a given persona. Repository sources are grouped
under `jni/` (Zygisk module), `native/` (CLI, CMake, shared headers), and `sh/`
(lifecycle, identity, helper, and debug scripts). `build.sh` flattens the shell
and data files into the module layout expected by Magisk, KernelSU, and APatch.

---

## Requirements

| Item | Version |
|------|---------|
| Android | 13+ (API 33+) |
| Root manager | KernelSU / Magisk >= 26100 / APatch |
| Zygisk provider | ZygiskNext, HMA-OSS, ReZygisk, NeoZygisk, or built-in Magisk Zygisk |
| ABI | arm64-v8a, armeabi-v7a, x86_64, x86 |

---

## Installation

1. Build the module (see *Build from Source*) or obtain a release zip named
   `sandboxid-vX.Y.Z-release.zip`.
2. Flash it via your root manager (KernelSU / Magisk / APatch) →
   **Modules** → **Install from storage**.
3. Reboot.
4. (Optional) Tap the module **Action** button in your root manager to run a
   full rotation, or configure `target.txt` first (see *Configuration*).

The module path is `/data/adb/modules/sandboxid`.

---

## Configuration

### `target.txt`

The list of packages the module acts on lives at:

```
/data/adb/modules/sandboxid/target.txt
```

- One package name per line.
- Blank lines and `#` comments are ignored.
- The file **ships empty**. An empty (or absent) `target.txt` means the module
  is idle — no application is modified. This is the no-op default path.
- Edit the file and save; the companion re-reads it on the next app spawn
  (mtime watch). Verify with:

  ```bash
  su -c 'sandboxid targets'
  ```

Your `target.txt` is preserved across reinstalls by `customize.sh`.

### `identity.prop`

Written by `sandboxid freshen`, read by the native property apply, the Zygisk
hooks, and `rotate_ids.sh`. Located at
`/data/adb/modules/sandboxid/identity.prop`. Format:

```
MODEL=Pixel 8
BRAND=google
MANUFACTURER=Google
```

Empty or absent `identity.prop` is handled gracefully (the module continues
without applying a persona). `rotate_ids.sh` adds `WIFI_MAC`,
`BLUETOOTH_ADDR`, and `BLUETOOTH_NAME` to the file as they are generated.

### `personas.tsv`

The pool `sandboxid freshen` picks a persona from. Located at
`/data/adb/modules/sandboxid/personas.tsv`; tab-separated, 10 columns:

```
model	device	product	board	platform	sdk	release	id	incremental	security_patch
```

`#`-prefixed lines are comments. The bundled file is a curated set of **stable**
Pixel builds — editing it (or dropping in your own rows) changes the pool
directly; no rebuild needed. If the file is missing or empty, the native binary
falls back to a small built-in list, so `freshen` always works.

`autopif.sh` (run automatically by `action.sh`, best-effort) refreshes this file
with the latest **canary** Pixel fingerprints scraped from Google, when the
device has `curl`/`wget`. It is a **no-op offline** and skips any device whose
SoC it can't map, so it never makes the pool inconsistent. See
[Credits](#credits--references).

---

## CLI Reference

### `sandboxid` (native)

```bash
su -c 'sandboxid <command>'
```

| Command | What it does |
|---------|--------------|
| `freshen` | Generate a new persona (identity + mount overlay) and wipe target app data |
| `status` | Print the current `identity.prop` |
| `rollback` | Restore the previous identity from backup |
| `lock` / `unlock` | Prevent / re-enable `freshen` (safety after setup) |
| `apply-boot` | Re-apply native props via `resetprop-rs` (used by `service.sh`) |
| `seed` | Fast bootstrap: identity + mount overlay only (used by `post-fs-data.sh`) |
| `targets` | List the active target list from `target.txt` |

### `rotate_ids.sh`

```bash
su -c 'sh /data/adb/modules/sandboxid/rotate_ids.sh <cmd>'
```

| Command | Applies | Needs reboot? |
|---------|---------|---------------|
| `all` | SSAID + GAID + wlan MAC + BT MAC + device name + applog (default) | Yes (SSAID) |
| `safe` | GAID + BT MAC + device name + applog (skips SSAID + wlan) | No |
| `ssaid` | Delete `settings_ssaid.xml` per user | Yes |
| `gaid [uuid]` | Set Google Advertising ID | No |
| `wlan-mac [xx:xx:...]` | Set `wlan0` MAC + wipe `WifiConfigStore` | No |
| `bt-mac [xx:xx:...]` | Set Bluetooth adapter MAC + `bt_config.conf` Address | No (toggle BT) |
| `device-name [name]` | Sync device/BT name to `identity.prop` MODEL | No |
| `applog [pkg]` | **Rotate** ByteDance AppLog IDs (bump APPLOG_EPOCH + wipe stale cache + force-stop; the zygisk L9 hook serves the new did/iid/ssid/openudid/clientudid/cdid in-process) | No |
| `applog-wipe [pkg]` | Wipe-only escape hatch (no rotation; forces SDK to re-register from server) | No |
| `status` | Read-only snapshot of all identifiers (never dumps AppLog values — privacy) | — |
| `help` | Print usage | — |

**`applog` in detail.** Apps built on the ByteDance **AppLog / RangersAppLog**
SDK (TikTok `com.ss.android.ugc.trill`, Douyin `com.ss.android.ugc.aweme`,
TikTok Global `com.zhiliaoapp.musically`, CapCut, Lark, and any third-party app
that ships `com.bytedance.applog`) cache a **server-issued** trio of identifiers
alongside the hardware fingerprint the module already spoofs:

- `device_id` (aka `did` / `bd_did`) — Snowflake 64-bit int (19 decimal
  digits; decodes as `(unix_ms << 22) | 22-bit random`), minted by the register
  endpoint `/service/2/device_register/` and pinned to the app install
- `install_id` (aka `iid`) — same Snowflake shape, rotates on reinstall,
  links to the current install
- `ssid` — server-side ID (same shape) that maps `device_id ↔ user_unique_id`
  even across logout / re-login
- `cdid` — RFC 4122 UUID v4 (locally generated), seeds the register call
- `clientudid` — RFC 4122 UUID v4
- `openudid` — 16 hex chars (legacy iOS UDID shape, Android SDK reuses)

These live in `shared_prefs/applog.xml`, `shared_prefs/snssdk_openudid.xml`,
`shared_prefs/bd_device_info.xml`, `files/bd_setting/{device_id, install_id,
openudid, clientudid}`, and `files/.cdid`.

SandboxID does **not** seed these files anymore. The zygisk module (L9)
spoofs them **in-process**: every read of an AppLog cache file is redirected
to memfd content derived deterministically from the persona identity +
package name + `APPLOG_EPOCH` from `identity.prop`.

`rotate_ids.sh applog` is the rotation command:

1. **Bump** — `APPLOG_EPOCH` in `identity.prop` is set to now (unix ms)
2. **Wipe** — every known AppLog cache file is removed (tar-backup first)
3. **Force-stop** — the target is killed so no warm process keeps using the
   old IDs in memory

---

## `identity.prop` schema

Written by `freshen`, read by native prop apply, Zygisk hooks, and
`rotate_ids.sh`. Located at `/data/adb/modules/sandboxid/identity.prop`.

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
| `ANDROID_ID` | `freshen` | Persona seed and SSAID rotation input |
| `GOOGLE_AID` | `freshen` | GAID |
| `WIFI_MAC` | `rotate_ids.sh` | Persisted wlan0 MAC |
| `BLUETOOTH_ADDR` | `rotate_ids.sh` | Persisted BT adapter MAC |
| `BLUETOOTH_NAME` | `rotate_ids.sh` | Optional override for device/BT name |

Use `identity_get KEY` / `identity_persist KEY VALUE` from `helpers.sh` for
programmatic access (atomic upsert via `awk` + rename).

---

## Build from Source

```bash
git clone https://github.com/Ilham311/sandboxid.git
cd sandboxid

export ANDROID_NDK_HOME=/opt/android-ndk-r26d

cp /path/to/resetprop prebuilt/resetprop-rs   # optional but recommended

curl -fsSL -o jni/zygisk.hpp \
  https://raw.githubusercontent.com/topjohnwu/zygisk-module-sample/master/module/jni/zygisk.hpp

./build.sh    # produces dist/sandboxid-<version>-{release,debug}.zip
```

Environment overrides:

| Var | Default | Meaning |
|-----|---------|---------|
| `VARIANT` | `both` | `release`, `debug`, or `both` |
| `MIN_SDK` | `26` | Native min API level |

Build with `-Wall -Wextra` per ABI. The `debug` variant enables verbose
`[D]` logs and on-device session capture under
`/data/adb/modules/sandboxid/debug/`.

---

## License

[MIT](./LICENSE) (c) 2026 Ilham311