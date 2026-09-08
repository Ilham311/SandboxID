# SandboxID

**Android device identifier privacy research & education module**

SandboxID is an open-source Zygisk module for studying and experimenting with
the device identifier fields that applications read on a device you own. It
keeps identity configuration inside the module and presents selected Android
`Build.*` and property values only inside configured target processes, so you
can observe how apps behave against varied device configurations and learn
Android internals.

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
- **Android internals learners** — explore how module-local identity and
  per-application Zygisk/JNI/Bionic/mount-namespace presentation work on a real
  device.

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

SandboxID uses a module-local identity plus one per-target presentation path:

1. **Module-local identity** — `identity.prop` stores the validated persona and
   operational flags under `/data/adb/modules/sandboxid`. Normal operation does
   not publish persona properties globally before Zygote or write framework
   Settings after boot.
2. **Per-target process presentation** — during Zygisk specialization, the
   companion serves one validated identity snapshot. JNI updates selected
   `Build.*` fields, Java and native hooks intercept `SystemProperties` and
   Bionic property reads, and synthetic `build.prop` files are bind-mounted only
   in that target's mount namespace. SDK level, ABI lists, preview state, media
   performance class, first API level, ISA, heap settings, runtime hardware,
   and other platform capabilities remain genuine.

The layers share a canonical persona where their scopes overlap. They do not
turn denied, empty, redacted, opt-out, or service-owned API results into a
synthetic success, and they do not claim to replace hardware-backed evidence.
On preview or unknown runtimes, preview capability state and hybrid
release/codename fields pass through unchanged. Only a verified stable runtime
(`preview_sdk=0`, `codename=REL`) receives persona release aliases.

### Native property callback contract

For genuine non-null Bionic `prop_info*` handles, callback reads preserve the
caller's cookie, property name, and serial and invoke the callback exactly once.
Unmapped values pass through byte-for-byte; mapped values are not truncated to
the legacy 92-byte getter buffer, and a hidden existing property completes once
with an empty value. Null callbacks and missing/null properties remain no-ops.
SandboxID never fabricates property handles or inspects private `prop_info`
layouts.

### Experimental native-file presentation

`SBX_NATIVE_READ=1` is the master gate. It is enabled only by the exact value
`1`; a missing, malformed, or different value leaves native-read presentation
disabled. The following child gates are independent and default to `0`:

- `SBX_PROC_VERSION` — presents `/proc/version`; `uname(2)` stays genuine.
- `SBX_MEMINFO` — presents selected `/proc/meminfo` content;
  `ActivityManager.MemoryInfo` stays genuine.
- `SBX_SYSFS_MAC` — presents allowlisted sysfs MAC files;
  `NetworkInterface` and `WifiInfo` stay genuine or redacted.
- `SBX_CPU_REVISION` — normalizes only aggregate CPU revision text; topology,
  features, per-core records, and Hardware text stay genuine.

These opt-ins are partial presentation controls, not full environment
virtualization. Disabled surfaces are not synthesized.

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
3. Reboot once.
4. Configure `target.txt` (see *Configuration*), then generate or select a
   module-local persona with the module Action if desired.

### Upgrade recovery

If an older build performed global property/Settings or identifier mutations,
install the corrected build and reboot once. Do **not** edit or delete
`settings_ssaid.xml` on a live system. Previous app-data, Settings, or identifier
mutations cannot be detected or automatically reversed by the module; restore
those from your own backup or platform-supported recovery path where available.

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

Written by `sandboxid freshen` and read by the per-target Zygisk/JNI/Bionic and
mount-namespace presentation layers. Located at
`/data/adb/modules/sandboxid/identity.prop`. Format:

```
MODEL=Pixel 8
BRAND=google
MANUFACTURER=Google
```

Empty or absent `identity.prop` is handled gracefully (the module continues
without applying a persona). Operational changes remain module-local until a
configured target starts.

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
| `freshen` | Generate and atomically store an exact-SDK module-local persona |
| `import <file>` | Validate and atomically import a generated module-local persona |
| `status` | Print the current `identity.prop` |
| `set-flag <key> <0\|1>` | Atomically set one validated `SBX_*` operational flag; restart the target process to apply |
| `rollback` | Restore the previous module-local identity |
| `lock` / `unlock` | Prevent / re-enable `freshen` (safety after setup) |
| `seed` | Validate or generate the module-local identity and per-target mount files |
| `targets` | List the active target list from `target.txt` |

### Android identifiers, app data, and permission boundary

Android 8+ scopes `Settings.Secure.ANDROID_ID` (SSAID) by signing key, user, and
device. There is no supported shell command to reset it, and SandboxID does not
hook or regenerate SSAID. It also does not write post-boot Settings, clear app
data, or provide an aggregate identifier-rotation workflow. Advertising ID and
other service-owned identifiers remain controlled by their platform services.
Do not modify `settings_ssaid.xml` while Android is running.

`pm clear <package>` deletes that package's complete user-data area, including
its login/session state and runtime-permission state; it is not a cache-only
operation. Persona Action, `freshen`, `import`, and `rollback` never invoke it,
never force-stop apps automatically, and never reset permission flags. Apps
control when they request a runtime permission again, and Android may suppress
a later permission dialog while the denial remains marked `USER_FIXED`.
Restart only the relevant target processes manually after changing persona.

For a previous build that performed those mutations, follow *Upgrade recovery*:
install the corrected build and reboot once. Historical data or identifier
changes cannot be automatically reversed.

---

## `identity.prop` schema

Written by `freshen` or Action, then read by the per-target native property,
Zygisk, and mount-namespace presentation layers. `ANDROID_ID`, `GOOGLE_AID`, and
other identifier-like fields in this file are profile metadata or desired values
for explicitly invoked `rotate_ids.sh` commands; they are not claims about the
corresponding service APIs. Located at `/data/adb/modules/sandboxid/identity.prop`.

| Key | Written by | Purpose |
|-----|-----------|---------|
| `MODEL` | `freshen` | `Build.MODEL`, `ro.product.model` (all partitions) |
| `BRAND` | `freshen` | `Build.BRAND`, `ro.product.brand` |
| `MANUFACTURER` | `freshen` | `Build.MANUFACTURER`, `ro.product.manufacturer` |
| `DEVICE` | `freshen` | `Build.DEVICE`, `ro.product.device`, `ro.build.product` |
| `PRODUCT` | `freshen` | `Build.PRODUCT`, `ro.product.name` |
| `BOARD`, `HARDWARE`, `BOARD_PLATFORM`, `SOC_*` | persona generator | Internal persona metadata only; runtime hardware properties and `Build` fields pass through genuine |
| `FINGERPRINT`, `ID`, `DISPLAY` | `freshen` | Build metadata |
| `SERIAL` | `freshen` | `Build.SERIAL`, `ro.serialno`, `ro.boot.serialno` |
| `RADIO` | `freshen` | `Build.RADIO`, `gsm.version.baseband` |
| `ANDROID_ID` | persona generator | Profile entropy used by local derivations; not an active `Settings.Secure.ANDROID_ID` hook |
| `GOOGLE_AID` | persona generator / `rotate_ids.sh` | Desired local GAID storage value; the service API and opt-out state remain service-owned |
| `WIFI_MAC` | `rotate_ids.sh` | Persisted wlan0 MAC |
| `BLUETOOTH_ADDR` | `rotate_ids.sh` | Persisted BT adapter MAC |
| `BLUETOOTH_NAME` | `rotate_ids.sh` | Optional override for device/BT name; if unset, uses `MODEL` |
| `SBX_NATIVE_READ` | generator / `set-flag` | Master native-read gate; defaults to `1`; `no_native_read` still forces it off per spawn |
| `SBX_PROC_VERSION` | generator / `set-flag` | Experimental `/proc/version` presentation; defaults to `0` |
| `SBX_MEMINFO` | generator / `set-flag` | Experimental `/proc/meminfo` presentation; defaults to `0` |
| `SBX_SYSFS_MAC` | generator / `set-flag` | Experimental sysfs MAC presentation; defaults to `0` |
| `SBX_CPU_REVISION` | generator / `set-flag` | Experimental aggregate CPU revision presentation; defaults to `0` |
| `SBX_HIDE` | generator / `set-flag` | Existing independent hide gate; defaults to `0` |

Operational flags are preserved across native `freshen`, `rollback`, multibrand
Action replacement, and reinstall migration. Shell code should use
`identity_get KEY` / `identity_persist KEY VALUE` from `helpers.sh`; the latter
collapses duplicate keys and replaces the file atomically. Interactive callers
should prefer the validated `sandboxid set-flag` command.

---

## Build from Source

```bash
git clone https://github.com/Ilham311/sandboxid.git
cd sandboxid

export ANDROID_NDK_HOME=/opt/android-ndk-r26d

# The repository must contain all four official Enginex0/resetprop-rs v0.6.0
# assets under prebuilt/; build.sh verifies prebuilt/resetprop-rs.sha256.
(cd prebuilt && sha256sum -c resetprop-rs.sha256)

./build.sh    # produces dist/sandboxid-<version>-{release,debug}.zip
```

Environment overrides:

| Var | Default | Meaning |
|-----|---------|---------|
| `VARIANT` | `both` | `release`, `debug`, or `both` |
| `MIN_SDK` | `26` | Native min API level (26 = Android 8; low floor so the .so loads across Android 12–16) |

Builds use the pinned `zygisk.hpp` commit/checksum in `build.sh` and the four
official Enginex0/resetprop-rs v0.6.0 assets: `arm64-v8a`, `armeabi-v7a`,
`x86_64`, and `x86`. Missing assets, unsupported installation ABI, absent
manifest entries, or SHA-256 mismatches abort packaging/installation. Runtime
property writes use only the selected bundled `bin/resetprop-rs`; there is no
Magisk/PATH `resetprop`, PATH `resetprop-rs`, or `setprop` fallback.

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

SandboxID uses root/Zygisk module APIs plus documented Android platform commands
for the explicit, separately requested shared-device operations listed above.
Normal persona generation and rollback do not clear application data, force-stop
applications, publish properties globally, or write framework Settings. External
Android and AOSP documentation used to explain identifier scope and property
behavior is linked above. The repository is MIT licensed; inspect your checkout's
history and dependency metadata for version-specific attribution.

---

## Scope & Limitations

### VD-Infos-oriented capability matrix

VD-Infos' public v2.13 documentation is used here as methodology evidence for
native callback reads; it is not evidence for unpublished source paths or
symbols. No v2.13 result fixture is claimed without an actual immutable report.

| Class | SandboxID behavior |
|-------|--------------------|
| Covered presentation | Selected `Build.*`, Java `SystemProperties` string/typed/genuine-handle reads, Bionic getter/read/callback paths, and allowlisted `build.prop` views |
| Conditional native coverage | PLT hooks cover `.so` files mapped during the one-time specialization scan; debug logs record basename plus device/inode and registration/commit counts |
| Stable-runtime fix | Persona release aliases are written only when genuine preview state proves `preview_sdk=0` and `codename=REL`; preview/unknown state passes through |
| Opt-in-only | `/proc/version`, selected `/proc/meminfo`, sysfs MAC, and aggregate CPU revision presentation are independent, default-off child gates |
| Preserve genuine/error state | Permission denial, null/empty/redacted/unknown results, opt-out, cancellation, callback/executor errors, dynamic services, runtime hardware, and capability APIs are not converted to success |
| Immutable evidence | Play Integrity, SafetyNet, Key/ID attestation, attested patch levels, verified-boot hashes, and other hardware/server-signed evidence remain genuine |
| Explicitly out of scope | Direct-syscall or `mmap` interception, broad loader interception, Java identifier entry hooks, package/signature/installer forgery, and additional concealment |

### Covered

- Validated presentation identity: selected `Build.*` strings/timestamps,
  string and typed `SystemProperties` reads, genuine nonzero `Handle` reads,
  native Bionic property reads, and per-target `build.prop` file views.
- Module-local persona generation, validation, atomic replacement, backup, and
  per-target mount artifact generation. Normal lifecycle work does not publish
  device-wide properties or write framework Settings.
- Explicit individual shared-device operations for local GAID Settings/XML,
  Wi-Fi/Bluetooth state, device name, boot count, carrier selection, and
  recognized AppLog caches. They are never coupled to persona generation and do
  not imply active Java identifier API hooks.
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

- Persona presentation is per configured target process; `seed`, `freshen`, and
  rollback only update module-local files. Broad property suppression is
  independently disabled unless canonical `SBX_HIDE=1`; missing, malformed, or
  zero flags pass genuine platform values through.
- `SystemProperties.native_find` and the long-handle getters are covered only
  for genuine nonzero handles returned by the platform. The module never
  fabricates handles or interprets private `prop_info` layouts. Direct reads of
  `/dev/__properties__` remain outside this boundary.
- Native PLT registration is a one-time scan of libraries mapped at
  specialization. Debug builds report each basename and device/inode pair plus
  registration/commit counts. Libraries loaded later are outside confirmed
  coverage; no `dlopen`/`android_dlopen_ext` interception is packaged.
- There is no Java-to-DEX build/embed/load pipeline in `build.sh` or
  `jni/CMakeLists.txt`; Java identifier entry hooks are therefore not active or
  packaged. Adding one is a separate opt-in phase requiring API-version,
  permission/redaction, cancellation, executor, and callback-error contracts.
- Android 8+ SSAID is system-managed and scoped by signing key, user, and
  device. SandboxID does not delete, back up, write, or claim to regenerate its
  live SettingsProvider storage. Local GAID writes are best-effort and do not
  guarantee what Google Play services returns or override its opt-out sentinel.
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
