# SandboxID — Project Context

## Overview
SandboxID is an open-source **Zygisk (Magisk/KernelSU) Android module** for
**device-identifier privacy research and education**. It lets a user study and
configure the `Build.*` / `SystemProperties` fields apps read on a device they
own, plus per-app identity values, through a native userland hook layer and a
CLI/shell layer. It runs **pre-zygote, before apps launch**, and ships **idle**:
`target.txt` is empty, so no app is modified until the user adds one.

## Architecture & Layout
- `jni/` — native C++ core, built with CMake + Android NDK:
  - `main.cpp` — Zygisk entry / module wiring
  - `sandboxid.cpp` — core identifier & per-app config logic
  - `companion.cpp` — Zygisk companion process
  - `config.hpp`, `pool.hpp` — config model & allocation helpers
  - `sbx_lsplant.hpp` — LSPlant-based ART (Java) hooking
  - `SandboxIDHook.java` — Java-side hook target
- Module lifecycle (root): `customize.sh`, `post-fs-data.sh`, `service.sh`, `action.sh`
- `webroot/` — KernelSU WebUI · `prebuilt/` — vendored artifacts
- `target.txt` — per-app target list (**ships empty**)
- `module.prop`, `update.json` — metadata / OTA
- `.github/workflows/build.yml` — CI build

## Build & Test
Requires Android NDK r26+.

```bash
export ANDROID_NDK_HOME=/path/to/ndk   # r26+, required
./build.sh                             # builds all ABIs into dist/
```

Env knobs (from `build.sh`):
- `MIN_SDK` — minimum SDK (default `33`)
- `VARIANT` — `both` (default) or a single variant
- `SBX_ENABLE_LSPLANT` — `ON` to compile the LSPlant Java-hook path (default `OFF`)
- ABIs: `arm64-v8a armeabi-v7a x86_64 x86` · Output: `dist/`

## Conventions
- Native code is C++ (`.hpp`/`.cpp`) under `jni/`, driven by `jni/CMakeLists.txt`.
- Follow current in-tree header names (recent refactors dropped `tt_`/`sbx_`
  prefixes, e.g. `lsplant.hpp`, `hook_dex.h`) — match neighbours, don't reintroduce old names.
- Shell scripts are bash with `set -euo pipefail`.
- Version lives in `module.prop` (`version=` / `versionCode=`); releases sync via CI.

## Safety & Scope
- Research/education module: keep changes neutral and user-controlled.
- The module must **ship idle** (`target.txt` empty) and modify no app unless the user opts in.
- Do not add default targets, telemetry, or network calls beyond the existing `updateJson` OTA check.
- Preserve pre-zygote ordering and keep the LSPlant opt-in flag default `OFF`.

## Maintaining This File
- Update this file whenever build flags, lifecycle scripts, or the `jni/` layout change.
- Commit context changes in the same PR as the code change so history stays auditable.
- Re-run `/context-audit` after major refactors to catch drift.
