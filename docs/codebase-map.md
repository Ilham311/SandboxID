# SandboxID Codebase Map

This is the responsibility and state-ownership map for the one-click identity architecture. Source is authoritative; use this ledger to identify direct consumers before changing a path, command, or protocol.

## Product topology

```text
customize.sh
  preserve validated state -> permissions -> ABI CLI link

post-fs-data.sh (pre-Zygote)
  restore flags -> effective-target gate -> seed -> apply-props

service.sh (framework ready)
  effective-target gate -> apply-boot -> optional debug workers

Action / WebUI
  preflight -> refresh -> prepare -> commit -> apply
  -> installed target reset -> committed-value rotation
  -> shared-epoch AppLog -> exact verify -> durable result

companion / target Zygisk process
  exact process target -> shared-locked bound identity read
  -> strict app validation -> hooks -> mounts -> optional hide
```

The primary outcome is one Action run that gives configured target apps a fresh, coherent presentation identity and privacy state. The boundary remains explicit: exact-process presentation is scoped, while properties, framework settings, target data, and several rotations are device-global or package-destructive. Hardware, attestation, immutable, and service-owned signals can remain genuine.

## Source responsibility

### Native

| Path | Responsibility |
|---|---|
| `jni/config.hpp` | Shared paths, IPC command IDs, identity blob cap, bind entries, exact I/O. |
| `jni/sbx_identity.hpp` | Strict identity parser/coherence, local-ID validation, flags, canonical serialization. |
| `jni/sbx_persona.hpp` | Bounded persona rows, exact-SDK selection, compiled SDK 31–36 catalog, source priority. |
| `jni/sbx_target.hpp` | Exact process syntax, normalized unique base packages, comments/whitespace handling. |
| `jni/sbx_transaction.hpp` | Canonical/pending metadata, owner metadata, provenance, digest validation. |
| `jni/sbx_sha256.hpp` | Header-pure SHA-256 used for identity/base/provenance binding. |
| `jni/sbx_property.hpp` | Mapped/hidden/pass property decisions and typed/callback helpers. |
| `jni/sbx_native_read.hpp` | Native read gates/transforms, deterministic AppLog IDs, path handling. |
| `jni/sbx_mountinfo.hpp` | Protected/trace mount selection for optional hide. |
| `jni/sandboxid.cpp` | Privileged canonical writer, transactions, overlays, apply, persona import, targets, and remaining local state. |
| `jni/companion.cpp` | Stable target cache, bound canonical read under shared state lock, peer-authorized mounts/hide. |
| `jni/main.cpp` | Target-process Zygisk lifecycle, Build/property/uptime/native-read hooks and IPC client. |
| `jni/CMakeLists.txt` | Android artifacts and optional header-pure host test targets. |

### Runtime shell and WebUI

| Path | Responsibility |
|---|---|
| `action.sh` | Whole-operation ownership, staged Action, accounting, rollback/forward recovery, protocol persistence. |
| `autopif.sh` | Always-attempted bounded Pixel OTA catalog/prefix acquisition and native persona import. |
| `rotate_ids.sh` | Standalone or committed-snapshot SSAID/GAID/boot rotation and reports. |
| `helpers.sh` | Mutation-owner checks, property/framework helpers, AppLog wipe/seed/probe, SELinux cleanup. |
| `customize.sh` | Verified preservation, ABI selection, permissions, resetprop-set integrity. |
| `post-fs-data.sh` | Pre-Zygote flag restore, seed, and property apply. |
| `service.sh` | Framework-ready apply and debug collection. |
| `selftest.sh` | On-device diagnostic `SELFTEST` protocol; not proof of target-process activation. |
| `build.sh` | Deterministic four-ABI staging/package policy; no persona network acquisition. |
| `validate.sh` | Aggregate non-device validation and explicit blocker reporting. |
| `webroot/app.js` | Privileged bridge, timeout/reconciliation, mutation serialization, parsers and mutations. |
| `webroot/index.html`, `style.css`, `theme-init.js` | CSP-safe static UI, accessible tabs/progress/theme. |

### Tests and package policy

| Path | Coverage |
|---|---|
| `tests/persona_test.cpp` | Persona parsing, exact SDK, source priority, compiled coverage, local values. |
| `tests/target_test.cpp` | Exact process preservation, package normalization/deduplication, rejection. |
| `tests/native_read_test.cpp` | Identity/property/native-read/AppLog/mount header-pure behavior. |
| `tests/action_test.sh` | Locking, precommit safety, rollback, forward recovery, accounting, protocol/exits. |
| `tests/autopif_test.sh` | Official-catalog join, bounded OTA metadata, non-repeat traversal, provenance/import, and last-known-good preservation. |
| `tests/rotation_test.sh` | Native standalone writes, snapshot mode, reports, failure restoration. |
| `tests/helpers_applog_test.sh` | Package normalization and per-package AppLog outcomes/shared epoch. |
| `tests/customize_test.sh` | Empty-target/mode/state preservation, ABI/resetprop policy. |
| `tests/webui_test.js` | Protocol, timeout cleanup, reconciliation, serialization, quoting and tabs. |
| `tests/package_manifest_test.sh` | Required runtime inputs, obsolete-input exclusion, resetprop set, blockers. |
| `tests/probe_expectations_test.py` | Fixed external-probe fixture policy; optional original-report integrity. |

## State ownership

| State | Writer | Readers and semantics |
|---|---|---|
| `identity.prop` + `identity.meta` | Native CLI only | Canonical validated pair. Metadata binds run/source/SHA-256. Companion reads the pair under shared `.state.lock`; module validates again. |
| `.bak` pair | Native commit/restore | Last validated state used for pre-irreversible restore. |
| `identity.pending` + `.meta` | Native `prepare`/`commit`/`abort` | Pending metadata also binds the canonical base-state digest. |
| `persona.override` | Operator/packaged input | One-shot exact-SDK source; removed only after successful commit. |
| `persona.cache` + `.meta` | Native `persona-import` | Persistent last-known-good candidate plus strict acquisition provenance. |
| `personas.tsv` | Optional reviewed package catalog | Extension tier; never replaces compiled coverage. |
| `target.txt` | Installer/WebUI/operator | Exact process list. Native parser exposes process and normalized package views. Stable effective empty is off. |
| `mount/` | Native checked staged publisher | Five source trees bind to eight destinations; tied to canonical identity verification. |
| `.state.lock` | Native mutation/read guard | Exclusive canonical writers, shared companion reader. |
| `.mutation.lock/owner` | Action or standalone shell mutation | PID/start-tick/kind/token binding; Action owners also bind run. Native validates inheritance. |
| `.action.lock/owner` | `action.sh` | Whole-run owner with the same live-process/run/token checks. |
| `debug/action.state` | Native `action-write` from Action | Versioned run/stage/timestamps/counters and terminal class. |
| `debug/action.result` | Native `action-write` from Action | One terminal `SBX_ACTION_V1 RESULT`, used for timeout reconciliation. |
| `debug/rotation.<run>` | `rotate_ids.sh` | Per-component `ok|failed|unsupported`, counts and reboot need, bound to run. |
| AppLog stores | Helpers/rotation | Wiped/seeded per installed unique package from one committed epoch. Hook synthesis still requires targeted effective native-read. |

Do not infer live values from checkout files. Runtime state is under `/data/adb/modules/sandboxid`.

## Identity and persona contract

Required presentation/build fields remain those enforced by `sbx_identity.hpp`; additionally the Action snapshot carries strictly validated `GOOGLE_AID`, `BOOT_COUNT`, and `APPLOG_EPOCH`. GAID is UUID-v4 or the zero sentinel. Explicit migration can drop recognized retired synthetic Wi-Fi/Bluetooth/SIM keys only after metadata has been validated against the untouched legacy bytes; canonical serialization cannot emit them again.

Persona source order is strict: override, cache, reviewed extension, compiled built-in. All candidates must match the exact runtime SDK. Compiled reviewed rows cover 31–36; unsupported SDK fails before mutation with an update request. After package classification, every otherwise-valid Action attempts `autopif.sh refresh` before native `prepare`. SDK 35/36 use official exact-release Android 15/16 Pixel OTA catalogs; older supported SDKs fail acquisition before networking because their historical version URLs redirect to a generic page. The adapter reads a bounded prefix of direct allowlisted OTA ZIPs, extracts the authoritative fingerprint and security patch, fills only pinned supported Pixel platform/SoC mappings, and submits one complete 16-column candidate with `pixel-ota-v1` provenance to native `persona-import`. A hash-bound prior cache device is excluded when a supported alternative is present, and refresh fails without publication when no alternative exists. Acquisition failure warns and falls through; it cannot erase the prior cache, override, canonical identity, reviewed extension, or compiled fallback.

## Action and result ownership

Preflight obtains both lock domains and validates targets/tools/mode/property backend/package inventory before identity or device mutation. It first uses bounded retries for the user-scoped and compatibility Package Manager inventories. If both global forms remain unavailable, each unique base package is checked through `dumpsys package <target>`, an independent service-dump route: exactly one matching package block with one unambiguous user-0 `installed=true|false` state, or one exact not-found marker, proves classification. Missing, denied, malformed, mismatched, or conflicting dump output is unknown and falls through to bounded exact package-name-filtered Package Manager queries. Failed command output and bounded dump diagnostics are retained in `debug/action.log`, including stdout-only Binder diagnostics; successful full dumps are not logged. An exact filtered `package:<target>` row means installed and a successful filtered query without that exact row means absent. Any target unresolved by every route fails preflight without mutation. Prepare writes only pending state. Commit validates run, pending hash, and unchanged base state; stages overlays, backs up old canonical state, activates overlays, publishes canonical metadata, then consumes an override.

Apply failure occurs before target/identifier/AppLog mutation and triggers restore, property/settings reapply, and verification. Required property writes remain fatal on backend failure. The explicitly classified OEM alias `ro.build.expect.baseband` is existing-only, so genuine absence is a successful skip while a failed attempted write remains fatal; `gsm.version.baseband` remains required and both aliases remain in overlays. Once target data or an external identifier store is touched, rollback would create a mismatched world, so Action keeps the committed identity and returns partial forward recovery. Terminal classes are `0`, `10`, `20`, `30`, `31`, `32`, `64`, `75`, and `127` as documented in `CLAUDE.md`.

Durability is rollback-on-return, not a journal. Pair and overlay routines detect ordinary write/fsync/close/rename failures and restore on the executing return path; power loss or SIGKILL between multi-file renames can still leave recovery work for a later process.

## Target, companion, and hooks

`target.txt` entries are exact process names such as `com.example.app:worker`; every desired process must be listed. Base packages are used only for package-manager/filesystem/AppLog effects and are deduplicated. The companion publishes only a stable target-file read; stable empty disables targeting while transient reload failure retains the prior in-memory set.

For a target GET, the companion takes a nonblocking shared state lock, reads identity/metadata together, validates binding, and distinguishes ready, missing, busy, and invalid. It invokes seed only for genuinely missing state. The module then strict-validates with genuine runtime SDK before activating any presentation. Invalid/unavailable state unloads the module and leaves the app genuine.

The hook layers remain Java Build/SystemProperties, Bionic property APIs, uptime, selected native reads/AppLog, build-property mounts, and optional hide. Synthetic Wi-Fi/Bluetooth identities, sysfs-MAC reads, and SIM/carrier presentation are absent. General framework `device_name` settings still follow persona `MODEL`, while defensive property hiding continues to remove genuine telephony/Wi-Fi/Bluetooth identifiers where covered. The hooks do not guarantee late-loaded import coverage or cross-lens hardware/attestation virtualization. IPC IDs remain `GET_IDENTITY=2`, `DO_MOUNTS=3`, `DO_HIDE=4` with local integer framing in `config.hpp`.

## Build and release boundary

Core runtime inputs are mandatory package files; `personas.tsv` is optional. Retired `carriers.tsv`/carrier state and obsolete `devices.tsv`/`device.identity` are rejected from package contracts. The complete resetprop binary/checksum/license trio is required if any member is present. `module.prop` absence is an explicit full-build blocker, and `jni/zygisk.hpp` absence is an explicit `main.cpp` syntax blocker.

The tracked release workflow runs on default-branch pushes/tags/manual dispatch and can commit metadata, tag, and publish. It does not run for pull requests. A feature-branch PR may be opened, but do not merge or trigger release automation without separate authorization.

## Verified and unverified boundaries

`bash validate.sh` is the authoritative safe aggregate gate. It never intentionally runs a real Action, rotation, remote refresh, Android settings/package mutation, or release operation. Header/mocked tests establish parser and orchestration contracts only. They do not prove real Zygisk/JNI/PLT registration, property service, framework Settings, SELinux contexts, mount namespaces/hide, OEM GMS layouts, Android ABI package execution, or power-loss recovery.