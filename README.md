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

The architecture is deliberately split so the Java hook layer and the shell
layer always report the same values for a given persona.

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
| `all` | SSAID + GAID + wlan MAC + BT MAC + device name (default) | Yes (SSAID) |
| `safe` | GAID + BT MAC + device name (skips SSAID + wlan) | No |
| `ssaid` | Delete `settings_ssaid.xml` per user | Yes |
| `gaid [uuid]` | Set Google Advertising ID | No |
| `wlan-mac [xx:xx:...]` | Set `wlan0` MAC + wipe `WifiConfigStore` | No |
| `bt-mac [xx:xx:...]` | Set Bluetooth adapter MAC + `bt_config.conf` Address | No (toggle BT) |
| `device-name [name]` | Sync device/BT name to `identity.prop` MODEL | No |
| `status` | Read-only snapshot of all identifiers | — |
| `help` | Print usage | — |

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
| `ANDROID_ID` | `freshen` | Per-app `Settings.Secure.ANDROID_ID` (hook layer) |
| `GOOGLE_AID` | `freshen` | GAID (also written by `set_gaid_value` if missing) |
| `WIFI_MAC` | `rotate_ids.sh` | Persisted wlan0 MAC |
| `BLUETOOTH_ADDR` | `rotate_ids.sh` | Persisted BT adapter MAC |
| `BLUETOOTH_NAME` | `rotate_ids.sh` | Optional override for device/BT name; if unset, uses `MODEL` |

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
| `MIN_SDK` | `26` | Native min API level (26 = Android 8; low floor so the .so loads across Android 12–16) |
| `SBX_ENABLE_LSPLANT` | `OFF` | Enable the experimental L3 LSPlant Java-method hook |

Build with `-Wall -Wextra` per ABI. The `debug` variant enables verbose
`[D]` logs and on-device session capture under
`/data/adb/modules/sandboxid/debug/`.

---

## Educational Resources

- **Android developer documentation — identifier best practices**:
  <https://developer.android.com/identity/user-data-ids> — official guidance on
  identifier scoping (Android ID, Advertising ID, SSAID) and when each applies.
- **Android Open Source Project — `Build`**:
  <https://source.android.com/docs/core/ota/modular-system> and the platform
  `Build` / `SystemProperties` sources explain how `Build.*` fields and the
  Android property system are populated and read at runtime.
- **AOSP property system**: the `system/core` `property_service` and
  `libcutils` `property_get` path show how native and Java code resolve
  `ro.*` properties, which is the mechanism SandboxID intercepts.
- **Academic background — user data sovereignty**: the principle that
  individuals should control the collection and use of their own device and
  behavioral data; see privacy-engineering literature on data minimization and
  user autonomy for further reading.

---

## Credits & References

SandboxID uses documented Android platform commands (`pm clear`,
`am force-stop`, `settings put`) and Magisk runtime APIs (`resetprop`, the
boot-stage contract), and adopts the `killall` process-stop technique from
PlayIntegrityFork (osm0sis, GPL-3.0). Full attribution, source links, and the
licensing note are in **[CREDITS.md](./CREDITS.md)**. No third-party source
code is bundled — only documented commands and techniques — so SandboxID
remains MIT.

---

## Scope & Limitations

### Covered

- Device fingerprint (`Build.*`, `SystemProperties.native_get*` typed variants)
- Per-app `ANDROID_ID` / SSAID override — **requires the experimental L3 hook**
  (disabled by default; see [Known limitations](#known-limitations))
- Google Advertising ID (`Settings.Global.advertising_id` + GMS `adid_settings.xml`)
- Wi-Fi MAC + `WifiConfigStore.xml` reset (with backup)
- Bluetooth adapter MAC (properties + `bt_config.conf` Address)
- Device / Bluetooth name synced to the persona `MODEL`
- Bind-mount overlay for `build.prop` files + `settings_secure.xml`
- Companion IPC protocol with hot-reloaded target list
- Crash watchdog and atomic configuration writes

### Out of scope

- Network-layer changes (VPN, residential IP)
- Play Integrity / SafetyNet bypass — use dedicated modules alongside if needed
- Sensor fingerprinting (planned)
- MediaDrm ID / GSF ID rotation (planned; need on-device tooling)

### Known limitations

Honest gaps in the current design — documented so you can reason about what a
detector still sees:

- **Per-app `ANDROID_ID` needs the L3 hook, which ships disabled.** `ANDROID_ID`
  (SSAID) is served over Binder by `SettingsProvider` in `system_server`, which
  caches the value at boot. A normal app calling `Settings.Secure.getString()`
  never reads the `settings_secure.xml` we bind-mount, so the overlay does not
  change the value it sees. Genuine per-app spoofing needs the L3 `getString`
  native hook (LSPlant + Dobby), which is **doubly disabled** in released builds:
  gated behind the `SBX_ENABLE_LSPLANT` compile flag (OFF by default) *and* its
  generated `hook_dex.h` is not checked in. Treat per-app `ANDROID_ID` spoofing
  as experimental / not active out of the box.

- **Two layers, two scopes.** The module has a *device-wide* layer (boot-time
  `resetprop` via `apply-boot`, active only when `target.txt` is non-empty) and a
  *per-app* layer (Zygisk hooks + `build.prop` bind-mounted into the target app's
  mount namespace). The device-wide layer affects *every* process; the per-app
  layer affects only the listed targets. Enabling one does not imply the other,
  and with an empty `target.txt` the module is fully idle by design.

- **`SystemProperties` `Handle` / `find()` fast path is not hooked.** We hook the
  typed `native_get*` entry points. Code that resolves a property `Handle` once
  (via `SystemProperties.find`) and reads through it, or reads
  `/dev/__properties__` directly, bypasses the JNI hook and sees the real value.
  The bind-mounted `build.prop` still covers file readers, but not the
  shared-memory fast path.

- **Non-target apps see a mixed identity.** The device-wide `resetprop` layer and
  the per-app bind-mount are independent. An app *not* in `target.txt` that reads
  props gets whatever the device-wide layer set (or the real values when idle)
  with no bind-mount overlay — so its `Build.*` and file-based props can disagree.
  Only listed target apps get a fully consistent persona.

SandboxID changes only identity strings. It does not modify hardware, the
framework boot path, or kernel state in ways that risk boot failure.

---

## Legal & Ethical Use

SandboxID is provided for **security research and educational purposes** on a
device you own. You are responsible for complying with the laws and terms that
apply to your use. Any use that violates a platform's Terms of Service is solely
your responsibility. The module is a learning and experimentation tool — apply
it with the same care you would give any root-level change to your device.

---

## License

[MIT](./LICENSE) (c) 2026 Ilham311
