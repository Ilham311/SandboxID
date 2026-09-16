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
4. **In-process file-read spoofing (L9)** — the Zygisk module PLT-hooks
   `read`/`open`/`openat`/`pread64`/`lseek`/`close` (plus the `_FORTIFY_SOURCE`
   `__open_2`/`__openat_2` variants) and serves synthetic content for the
   hardware-exposing files an app can read, per file descriptor so that
   chunked, repeated, rewound, or concurrent reads all stay consistent:

   - `/proc/version` — a kernel version string synthesized from the persona's
     release/platform/build number.
   - `/proc/meminfo` — `MemTotal` for the persona's model; for a model not in
     the RAM table, the real total is rounded **up** to the nearest marketing
     GB tier (the exact kB value is itself a fingerprint), never leaked as-is.
   - `/proc/cpuinfo` — the `Hardware` line rewritten for Qualcomm/MediaTek SoCs,
     or removed for Tensor, where present.
   - `/proc/sys/kernel/random/boot_id`, `/sys/class/net/wlan0|p2p0/address`,
     `/sys/fs/selinux/enforce`.
   - the ByteDance AppLog cache files (see `rotate_ids.sh applog` below).

   If the identity is incomplete for a file, the hook fails **closed** (serves
   EOF) rather than letting real bytes through. PLT hooking only sees calls
   that cross a library boundary, so reads from an app's own `.so` (not loaded
   yet at hook-install time) are covered by the on-disk pre-warm in layer 3
   instead, which writes the same deterministically-derived values.

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
`/data/adb/modules/sandboxid/personas.tsv`; tab-separated, **10 required columns
plus 6 optional ones (up to 16)**:

```
# required (1-10):
model	device	product	board	platform	sdk	release	id	incremental	security_patch
# optional (11-16) — brand identity for NON-Google devices:
brand	manufacturer	marketname	soc_manufacturer	soc_model	radio
```

Google/Tensor Pixel rows omit columns 11-16: `brand` defaults to `google`,
`manufacturer` to `Google`, and the SoC/model strings are derived from the
platform. Non-Tensor rows **must** supply at least `brand` and `soc_model`, or
the row is skipped with a warning (a persona can not be coherent without them).

`#`-prefixed lines are comments. The bundled file is a curated set of **stable**
Pixel builds plus a set of real-device multi-brand rows — editing it (or dropping
in your own rows) changes the pool directly; no rebuild needed. If the file is
missing or empty, the native binary falls back to a small built-in list, so
`freshen` always works.

### `devices.tsv` and the multi-brand flow

`devices.tsv` is the **real-device identity source** used by `autopif.sh device`
(the script's default mode, also run by `action.sh`, best-effort). It is a
tab-separated table of real, non-Pixel devices (Samsung / Xiaomi / POCO / OPPO /
vivo / Redmi / Infinix). From it, `autopif.sh` generates a complete
`identity.prop` written to `device.identity`, which `freshen` applies when
present. `autopif.sh fetch` instead writes a single fresh canary **Pixel**
persona to `persona.override`, which takes precedence over the pool.

Neither file is rewritten into `personas.tsv` — the pool is a stable, shipped
fallback, and the generated override is applied on top of it. Both scripts are a
**no-op offline** (they need `curl`/`wget`) and skip any device whose SoC they
can't map, so they never make the identity inconsistent. See
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

These files are served from **two layers that must agree**:

1. **In-process (authoritative)** — the zygisk module (L9) intercepts every
   read of an AppLog cache file and redirects it to memfd content derived
   deterministically from `fnv1a(FINGERPRINT | SERIAL | ANDROID_ID | pkg)`
   + `APPLOG_EPOCH` from `identity.prop`. This is what any running app
   actually observes.
2. **On-disk (best-effort pre-warm)** — `rotate_ids.sh applog` also *writes*
   the same values to the cache files via the `applog_seed` helper, so an app
   that reads them through a code path the hook does not cover (a backup /
   restore, a direct `cat`, a non-zygote child) still sees the rotated IDs
   instead of the stale ones.

Both layers derive from the same seed and epoch — the `applog-ids` CLI
command is the single source of truth, and `applog_seed` is a plain consumer
of it — so an on-disk file and an in-process read can never disagree. If the
CLI binary or the write is unavailable, `applog_seed` fails closed (it logs a
warning and leaves no root-owned file behind); the hook still spoofs
in-process.

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
git clone https://github.com/Ilham311/SandboxID.git
cd SandboxID

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

### Host verification suites

The deterministic core that both the Zygisk hook layer and the CLI build on can
be verified **without a device or an NDK** — any C++20 host compiler works:

```bash
bash tests/host/run.sh        # or: CXX=g++ bash tests/host/run.sh
```

Two suites run, and both must pass:

- `sbx_pure` — the pure functions: `classify()`, MAC validity, AppLog ID
  determinism across both entry points, `patch_applog_xml` /
  `patch_meminfo` / `patch_cpuinfo`, and the uuid/hex/snowflake helpers.
- `sbx_hook_sm` — the **real** per-fd synth state machine from
  `jni/module_hooks.cpp`, which the suite includes directly so the code under
  test is production code rather than a copy. It opens the host's actual
  `/proc/meminfo` and `boot_id` and makes the hook overwrite their real bytes,
  covering full read + EOF, `lseek` rewind, chunked reads, `pread64` (which
  must not advance the sequential cursor), relative-path `openat`, and
  fail-closed behaviour when the identity is incomplete.

The same suites run in CI (`host-tests` job). They are deliberately outside the
release-critical path, so an environment quirk on a CI runner cannot block a
release.

---

## License

[MIT](./LICENSE) (c) 2026 Ilham311