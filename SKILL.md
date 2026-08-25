---
name: sandboxid
description: Understand and work on SandboxID, a Zygisk (Magisk/KernelSU/APatch) Android module for device-identifier privacy research and education. Use this skill when reading, extending, or debugging the native (jni/) hook layer, the root-side shell lifecycle scripts, the companion IPC protocol, or the persona/identity data model.
---

# SandboxID

## Overview

SandboxID is an open-source Zygisk module that lets a device owner study and
override the `Build.*` / `SystemProperties` fields and per-app identity data
that Android apps read, plus native `/proc` and `/sys` reads. It runs
**pre-zygote**, before app processes spawn, and **ships idle**: `target.txt`
is empty, so no app is touched until the user opts in.

Read `CLAUDE.md` and `README.md` first — this file summarizes how to navigate
and safely modify the codebase; those two are the source of truth for
behavior and CLI/config schema.

## Architecture (three cooperating layers)

1. **Zygisk hook layer** (`jni/main.cpp`, `jni/sandboxid.cpp`,
   `jni/sbx_lsplant.hpp`, `jni/sbx_native_read.hpp`) — runs inside the
   zygote/app process. Patches Java `Build.*` fields, hooks
   `SystemProperties.native_get*`, and (native, `sbx_native_read.hpp`) hooks
   `open`/`openat`/`fopen`-family calls to redirect `/proc` and `/sys` reads
   for spoofed identifiers into an anonymous memfd.
2. **Companion process** (`jni/companion.cpp`) — root-side process that
   serves per-package identity blobs to the Zygisk hook over the Zygisk
   socket, hot-reloads `target.txt` (mtime watch), and bind-mounts a
   synthetic `build.prop` tree + `settings_secure.xml` into the target app's
   mount namespace.
3. **CLI / shell lifecycle** — the native `sandboxid` binary (built from
   `jni/`) plus `rotate_ids.sh`, `customize.sh`, `post-fs-data.sh`,
   `service.sh`, `action.sh`, `helpers.sh`. Regenerates personas, applies
   native properties via `resetprop-rs` (`prebuilt/resetprop-rs`), and
   rotates shell-layer identifiers (SSAID, GAID, Wi-Fi/Bluetooth MAC, device
   name).

Key data files (all under `/data/adb/modules/sandboxid/` on-device, and at
repo root during development):

- `target.txt` — one package per line; empty by default (idle module).
- `identity.prop` — the active persona's key/value identity blob; see the
  schema table in `README.md`.
- `personas.tsv` — tab-separated pool of candidate device fingerprints (10
  columns); `autopif.sh` can refresh it from public canary data, best-effort
  and offline-safe.

## Where to look for what

| Task | Files |
|---|---|
| Zygisk entry / module wiring | `jni/main.cpp` |
| Core identifier & per-app config logic | `jni/sandboxid.cpp` |
| Companion (root-side) process, IPC protocol | `jni/companion.cpp` |
| Config model / allocation helpers | `jni/config.hpp` |
| Native `/proc`,`/sys` file + property read hooks | `jni/sbx_native_read.hpp` |
| ART (Java) hooking via LSPlant (opt-in, `SBX_ENABLE_LSPLANT`) | `jni/sbx_lsplant.hpp`, `jni/SandboxIDHook.java` |
| Build system | `jni/CMakeLists.txt`, `build.sh` |
| Module lifecycle (root) | `customize.sh`, `post-fs-data.sh`, `service.sh`, `action.sh` |
| Shared shell helpers (logging, backups, user enumeration) | `helpers.sh` |
| Identifier rotation (SSAID/GAID/MAC/name) | `rotate_ids.sh` |
| Persona pool refresh (canary fingerprints) | `autopif.sh` |
| WebUI (KernelSU) | `webroot/` |
| Vendored binaries | `prebuilt/` (e.g. `resetprop-rs`) |
| Native tests | `tests/native_read_test.cpp` |
| Metadata / OTA | `module.prop`, `update.json` |
| CI | `.github/workflows/build.yml` |

## Build & test

```bash
export ANDROID_NDK_HOME=/path/to/ndk   # r26+, required
./build.sh                             # builds all ABIs into dist/
```

Env knobs (`build.sh`):
- `MIN_SDK` — minimum SDK (default `33`)
- `VARIANT` — `both` (default) or a single variant
- `SBX_ENABLE_LSPLANT` — `ON` to compile the LSPlant Java-hook path (default `OFF`)
- ABIs: `arm64-v8a armeabi-v7a x86_64 x86` · Output: `dist/`

Native unit tests live in `tests/` (see `tests/native_read_test.cpp` for the
file-redirect / property-hook logic).

## Conventions & invariants to preserve

- Native code is C++ under `jni/`, driven by `jni/CMakeLists.txt`; no
  exceptions/RTTI (`-fno-exceptions -fno-rtti`), build with `-Werror` clean.
- Follow current in-tree header naming (no `tt_`/`sbx_` prefix reintroduction
  where already dropped) — match neighboring files.
- Shell scripts are POSIX-ish `sh`/`bash` with `set -euo pipefail`; use
  `helpers.sh` functions (`identity_get`, `identity_persist`, `log_*`,
  `mask_id`, `se_permissive`/`se_restore`) instead of re-implementing them.
- Time source for any monotonic/interval logic must be `CLOCK_BOOTTIME` only.
- Companion/hook degradation must be graceful: a missing `exemptFd` or failed
  hook must not crash the host process.
- The module **must ship idle**: empty `target.txt` ⇒ no app is modified. Do
  not add default targets, telemetry, or network calls beyond the existing
  `update.json` OTA check.
- The LSPlant opt-in flag defaults `OFF`; don't flip that default.
- Version lives in `module.prop` (`version=` / `versionCode=`); releases sync
  via CI — don't hand-edit release artifacts.
- When native file-redirect hooks emulate reads (e.g. `/proc`, `/sys` via
  memfd), keep spoof handling defensive: guard against uninitialized/failed
  reads before treating buffers as valid C strings, and consider anti-tamper
  detection vectors (e.g. `readlink /proc/self/fd/N`, LFS `*64` symbol
  variants such as `open64`/`openat64`/`fopen64`) when adding or reviewing
  hook coverage.

## Safety & scope

This is a research/education module — keep changes neutral, transparent, and
user-controlled. Do not add anything that acts on an app without the user
explicitly listing it in `target.txt`. See "Known limitations" in
`README.md` for gaps that are intentionally documented rather than silently
patched (per-app `ANDROID_ID` needs the disabled-by-default L3 hook, the
`SystemProperties` `Handle`/`find()` fast path isn't hooked, etc.) — don't
claim these are fixed unless the underlying limitation is actually resolved.

## Maintaining this file

Update `SKILL.md` (and `CLAUDE.md`) whenever the `jni/` layout, build flags,
lifecycle scripts, or data-file schema change, in the same PR as the code
change.
