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

SandboxID combines three cooperating phases:

1. **Pre-Zygote presentation properties** — `post-fs-data.sh` validates or seeds
   `identity.prop`, then runs `sandboxid apply-props` before Zygote caches
   `Build.*`. These are presentation strings; SDK level, ABI lists, preview
   state, media performance class, first API level, ISA, heap settings, and
   other runtime capabilities remain sourced from the real platform.
2. **Per-target process hooks** — during Zygisk specialization, the companion
   serves one validated identity snapshot. The module updates presentation
   `Build.*` fields, intercepts string/typed/Handle `SystemProperties` reads and
   native Bionic property reads, and bind-mounts synthetic `build.prop` files
   into that target's mount namespace.
3. **Post-boot lifecycle work** — `service.sh` runs `sandboxid apply-boot` only
   after Android reports boot completion. This refreshes mount files and
   framework device-name Settings; `rotate_ids.sh` can separately perform
   explicit, device-level storage rotations such as SSAID regeneration, local
   GAID storage, and Wi-Fi/Bluetooth state changes.

The layers share a canonical persona where their scopes overlap. They do not
turn denied, empty, redacted, opt-out, or service-owned API results into a
synthetic success, and they do not claim to replace hardware-backed evidence.

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

`autopif.sh device` (run by `action.sh`) chooses uniformly by brand from
`devices.tsv`, but only from rows whose SDK exactly matches the real runtime
SDK. If the runtime SDK cannot be read or the database has no exact match,
generation stops with an actionable error instead of relabelling an
incompatible persona. The separate `autopif.sh fetch` command can refresh the
optional Pixel canary override when network tools are available; native
validation still rejects an override whose SDK differs from the runtime.

---

## CLI Reference

### `sandboxid` (native)

```bash
su -c 'sandboxid <command>'
```

| Command | What it does |
|---------|--------------|
| `freshen` | Generate an exact-SDK persona, apply presentation properties, refresh overlays/Settings, and wipe target app data |
| `status` | Print the current `identity.prop` |
| `rollback` | Restore the previous identity and apply the same phases |
| `lock` / `unlock` | Prevent / re-enable `freshen` (safety after setup) |
| `apply-props` | Apply presentation properties; boot scripts run this before Zygote |
| `apply-boot` | Apply post-boot framework Settings and refresh mount files |
| `seed` | Validate or generate identity and mount files before `apply-props` |
| `targets` | List the active target list from `target.txt` |

### `rotate_ids.sh`

```bash
su -c 'sh /data/adb/modules/sandboxid/rotate_ids.sh <cmd>'
```

| Command | Applies | Needs reboot? |
|---------|---------|---------------|
| `all` | SSAID storage regeneration + local GAID write + wlan/BT MAC + device name + AppLog (default) | Yes (SSAID regeneration) |
| `safe` | Local GAID write + BT MAC + device name + AppLog (skips SSAID + wlan) | No |
| `ssaid` | Back up and delete `settings_ssaid.xml`; Android regenerates it after reboot | Yes |
| `gaid [uuid]` | Best-effort local Settings/XML GAID write; the all-zero sentinel keeps local opt-out flags, and the command does not hook the advertising-ID API | No |
| `wlan-mac [xx:xx:...]` | Set `wlan0` MAC + wipe `WifiConfigStore` | No |
| `bt-mac [xx:xx:...]` | Set Bluetooth adapter MAC + `bt_config.conf` Address | No (toggle BT) |
| `device-name [name]` | Sync device/BT name to `identity.prop` MODEL | No |
| `applog [pkg]` | Rotate local ByteDance AppLog cache values served by the native-read layer | No |
| `applog-wipe [pkg]` | Wipe known AppLog caches without rotating the persona epoch | No |
| `status` | Read-only snapshot of all identifiers (never dumps AppLog values — privacy) | — |
| `help` | Print usage | — |

**`applog` in detail.** Some apps built on ByteDance **AppLog /
RangersAppLog** keep application-owned caches containing keys such as
`device_id`/`did`, `install_id`/`iid`, `ssid`, `cdid`, `clientudid`, and
`openudid`. Their source, format, and lifecycle vary by SDK and service version;
some may be assigned or reconciled remotely rather than controlled by the local
cache. SandboxID recognizes only the explicitly listed local files and does not
claim that changing them changes a service's server-side identity, account
state, registration, or relinking behavior.

The native-read layer can present deterministic per-package cache values from
the persona identity, package name, and `APPLOG_EPOCH` in `identity.prop`:

- recognized `shared_prefs/{applog,snssdk_openudid,snssdk_did,bd_device_info}.xml`
  reads patch only known identifier values and preserve unrelated XML entries;
- recognized `files/bd_setting/*` and `files/.cdid` pure reads receive bounded
  synthetic text.

`rotate_ids.sh applog` bumps `APPLOG_EPOCH`, backs up and removes recognized
cache files, and force-stops the selected targets so a warm process does not
keep old in-memory values. `applog-wipe` performs only the backup/removal step.
These are local privacy-testing controls, not a promise about remote
registration, relinking, or service acceptance.

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
| `ANDROID_ID` | persona generator | Profile entropy used by local derivations; not an active `Settings.Secure.ANDROID_ID` hook |
| `GOOGLE_AID` | persona generator / `rotate_ids.sh` | Desired local GAID storage value; the service API and opt-out state remain service-owned |
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
`am force-stop`, `settings put`) and root/Zygisk module APIs. External Android
and AOSP documentation used to explain identifier scope and property behavior
is linked above. The repository is MIT licensed; inspect your checkout's
history and dependency metadata for version-specific attribution.

---

## Scope & Limitations

### Covered

- Validated presentation identity: selected `Build.*` strings/timestamps,
  string and typed `SystemProperties` reads, genuine nonzero `Handle` reads,
  native Bionic property reads, and per-target `build.prop` file views.
- Pre-Zygote property publication plus checked per-process Build fallback for
  warm/vendor-cached paths.
- Exact-runtime-SDK persona selection. ABI arrays, `SDK_INT`, SDK extensions,
  preview state, first API level, Zygote/native bridge/ISA/heap configuration,
  and media performance class remain real runtime capabilities.
- Relative pure-read `openat` path resolution for the small native-read
  allowlist, with fail-open behavior on ambiguity.
- Optional aggregate `/proc/cpuinfo` revision normalization when
  `SBX_CPU_REVISION=1`; per-core records, topology, features, and Hardware lines
  remain byte-for-byte intact.
- Device-level lifecycle actions for SSAID storage regeneration, local GAID
  Settings/XML writes, Wi-Fi/Bluetooth state, device name, and recognized
  AppLog caches. These actions do not imply active Java identifier API hooks.
- Companion IPC with atomic target-list hot reload, crash logging, and atomic
  configuration writes.

### Intentionally not packaged or changed

- Java entry hooks for per-app SSAID/`ANDROID_ID`, advertising ID, App Set ID,
  AdServices ID, Telephony APIs (including `getSimCarrierId`), or Wi-Fi APIs.
- Forgery of Play Integrity, SafetyNet, Key/ID attestation, attested OS/vendor
  patch levels, verified-boot hashes, package/signature/installer identity, or
  any other hardware/server-signed evidence.
- Conversion of permission denial, empty/null, redacted/unknown, opt-out,
  cancellation, callback/executor error, or dynamic service state into a
  synthetic successful identifier.
- Network-layer identity changes, direct-syscall or `mmap` interception,
  directory enumeration, broad `dlopen` interception, or new root/mount/maps
  concealment.

### Known limitations

- Presentation properties are device-wide once `apply-props` runs, while
  per-process Build/property/file hooks are limited to `target.txt`. Keep the
  list intentional; an empty list leaves the module idle.
- `SystemProperties.native_find` and the long-handle getters are covered only
  for genuine nonzero handles returned by the platform. The module never
  fabricates handles or interprets private `prop_info` layouts. Direct reads of
  `/dev/__properties__` remain outside this boundary.
- SSAID deletion is a reboot-time regeneration action, not a per-app API
  override. Local GAID writes do not guarantee what Google Play services
  returns and must not override its opt-out sentinel.
- Carrier selection presents GSM operator properties. `GSM_CARRIER_ID` is
  retained as profile metadata, but `TelephonyManager.getSimCarrierId()` is not
  hooked by this build.
- Native file presentation covers recognized pure-read `open`/`openat`/`fopen`
  paths. Read-write or memory-mapped stores such as MMKV pass through.
- Signed/hardware-backed attestation values remain genuine. The immutable probe
  expectation fixture classifies attested vendor patch, OS patch, and vbmeta
  hash mismatches as `EXPECTED_IMMUTABLE`, not failures to spoof.
- On-device reboot and full probe verification are still required for each ROM
  and Zygisk provider; host tests cannot prove Android lifecycle timing.

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
