# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Product goal and safety boundary

SandboxID's primary goal is **one click / one Action run that gives every configured target app a new coherent presentation identity and privacy state**. Judge changes by the complete Action journey, not by an isolated hook or command. One successful run selects an exact-SDK persona, prepares and commits one identity, applies properties/settings, resets installed target apps, rotates device-impacting stores from committed values, seeds AppLog from one shared epoch, verifies the committed run, and reports an honest durable result.

The module is a pragmatic presentation/privacy and fresh-device testing layer. It is not hardware virtualization, an attestation bypass, or proof that every service-owned/immutable surface changed. Exact process names scope Zygisk presentation; some Action effects (properties, framework settings, package data, SSAID, and AppLog state) are global or device-impacting. Never run Action or rotation against a real device as a validation step.

## Repository state and hard blockers

The working architecture intentionally does not restore files deleted by `8430f87` without separate authorization. In particular:

- `module.prop` is absent, so `build.sh` must refuse a full Android/module build.
- `jni/zygisk.hpp` is generated/downloaded and gitignored; `jni/main.cpp` syntax remains explicitly blocked until the pinned header is available.
- `personas.tsv` is an optional reviewed convenience catalog. Runtime persona support does not depend on it: compiled reviewed personas cover SDK 31–36.
- Synthetic Wi-Fi/Bluetooth addresses, Bluetooth names, sysfs-MAC presentation, and SIM/carrier profiles are retired. Legacy keys may be dropped only by the explicit native migration path; defensive hiding of genuine OEM identifiers remains.
- `devices.tsv`, `device.identity`, and build-time persona refresh are obsolete and must not return to runtime/package contracts.
- `target.txt` is tracked and intentionally empty, which is the fail-safe off state.
- The large `session-*.log` is device evidence, not source; use narrow searches or `summarize.sh`.

## Safe development commands

Run from the repository root:

```bash
# Aggregate non-device gate
bash validate.sh

# Native spot checks not requiring zygisk.hpp
clang++ -std=c++20 -fsyntax-only -Wall -Wextra -Ijni jni/companion.cpp
clang++ -std=c++20 -fsyntax-only -Wall -Wextra -Ijni jni/sandboxid.cpp

# Header-pure host suites (Termux needs an executable temp directory)
TEST_TMP="${TMPDIR:-$PWD/.claude-tmp}"
clang++ -std=c++20 -Ijni -o "$TEST_TMP/sbx_persona_test" tests/persona_test.cpp && "$TEST_TMP/sbx_persona_test"
clang++ -std=c++20 -Ijni -o "$TEST_TMP/sbx_target_test" tests/target_test.cpp && "$TEST_TMP/sbx_target_test"

# WebUI checks
npx=false # no dependency installation is needed
node --check webroot/theme-init.js
node --check webroot/app.js
node tests/webui_test.js

# Shell syntax and whitespace
for f in $(git ls-files '*.sh'); do
  if grep -q '^#!/.*bash' "$f"; then bash -n "$f"; else sh -n "$f"; fi
done
git diff --check

# Full Android build only when explicitly requested and inputs exist
ANDROID_NDK_HOME=/path/to/android-ndk VARIANT=both ./build.sh
```

`validate.sh` runs debug/release native syntax where possible, tracked shell and JavaScript syntax, C++/Python host suites, mocked Action/acquisition/rotation/AppLog/installer tests, source/package policy checks, and whitespace checks. ShellCheck is skipped locally when unavailable. Host success does **not** prove Zygisk/JNI/PLT behavior, property service, framework Settings, SELinux, namespaces, mount/hide behavior, OEM stores, or four-ABI packaging.

## Authoritative state and transaction model

The native CLI is the only canonical identity writer. Coordinated files are:

- `identity.prop` + `identity.meta`: current canonical identity and SHA-256/run/source binding.
- `identity.prop.bak` + `identity.meta.bak`: last validated backup pair.
- `identity.pending` + `identity.pending.meta`: prepared candidate, including a digest of the canonical base state.
- `persona.override`: exact-SDK one-shot persona; consumed only after successful identity commit.
- `persona.cache` + `persona.cache.meta`: persistent last-known-good remote candidate and validated provenance.
- `mount/`: checked staged build-property overlays.
- `.state.lock`: native canonical-state `flock` domain.
- `.mutation.lock/owner`: shell/native mutation owner, bound to PID, `/proc` start ticks, kind, run (for Action), and token.
- `.action.lock/owner`: whole-Action owner.
- `debug/action.state` and `debug/action.result`: durable Action progress and terminal protocol.

Canonical metadata is version 1 with `run`, `source`, and `identity_sha256`. Pending metadata additionally requires `base_state_sha256`. Publication uses checked write, chmod where required, file fsync, close, same-directory rename, and parent-directory fsync. Canonical/metadata pairs and overlays use rollback-on-return, but there is no journal: power loss or SIGKILL between multi-file operations is not guaranteed atomic.

Persona priority is deterministic by source tier:

1. valid exact-SDK `persona.override`;
2. valid exact-SDK persistent `persona.cache`;
3. valid exact-SDK reviewed `personas.tsv` extension;
4. compiled reviewed exact-SDK persona;
5. unsupported SDK fails before mutation and asks for a module update.

Invalid or wrong-SDK optional sources warn and fall through. After package classification, every otherwise-valid Action invokes `autopif.sh refresh` before native `prepare`; acquisition failure is one warning and offline selection continues. SDK 35/36 use the official exact-release Android 15/16 Pixel OTA catalogs; older supported SDKs fail acquisition before networking because their historical version URLs redirect to a generic page. The adapter uses bounded allowlisted HTTPS, joins supported Pixel models to direct `dl.google.com` OTA URLs, and extracts the authoritative `post-build` fingerprint plus `post-security-patch-level` from a bounded prefix. It submits exactly one complete exact-SDK 16-column candidate with `pixel-ota-v1` provenance to native `sandboxid persona-import`. A hash-bound prior cache device is an exclusion constraint: traversal omits it when a supported alternative exists, and refresh fails without publication when no alternative exists. Every network, parse, mapping, schema, SDK, or import failure preserves the last-known-good cache, override, and canonical identity, so reviewed extension/compiled sources remain available.

## Action state machine

`action.sh` owns the complete one-click flow:

```text
preflight -> refresh -> prepare -> commit -> apply -> targets
          -> rotate -> applog -> verify -> result
```

Preflight validates ownership, tools, unlocked mode, targets, property backend, package inventory, and an offline persona source. Exact target process entries are preserved for Zygisk; native `targets --packages` returns unique normalized base packages for `pm`, `am`, filesystem, and AppLog operations. Installed/absent packages are classified before commit.

No package data, identifier store, or AppLog state is mutated when preflight, prepare, commit, or initial apply fails. A commit/apply failure restores and reapplies the old identity before irreversible work; after any irreversible target/rotation mutation, failures use forward recovery and keep the new canonical identity. Every installed target, rotation component, and AppLog package contributes to the result.

Machine records are tab-delimited `SBX_ACTION_V1`; human-readable Indonesian logs are supplementary. Exit classes are:

- `0`: complete, no reboot;
- `10`: complete, reboot required;
- `20`: committed, post-commit work partial;
- `30`: failed before commit, canonical state unchanged;
- `31`: old identity restored and reapplied;
- `32`: persistence/rollback/coherence degraded or unproven;
- `64`: invalid configuration/usage;
- `75`: mutation or Action owner is busy;
- `127`: required executable unavailable.

A bridge timeout means unknown outcome, never permission to retry. Resolve the matching run through durable state/result.

## Boot and native runtime

Boot ordering is deliberate:

- `post-fs-data.sh`: restore operational flags through native `set-flag`; if targets are effectively empty, stop; otherwise `seed` then pre-Zygote `apply-props`.
- `service.sh`: wait for framework boot, then `apply-boot` for effective targets; debug workers are separate. Its hard-coded `/cache/sandboxid-boot.log` behavior remains an integration caveat.
- Companion: exact-process target lookup, shared-locked identity/metadata read, seed only when state is genuinely missing, then send the bound snapshot.
- Zygisk module: strict app-side validation, Java Build/property hooks, uptime, AppLog presentation, mount overlays, and optional hide. Broad Bionic property/native-read PLT registration is retained but forced off pending a separately reviewed manager-compatible path.

Keep IPC IDs/framing in `jni/config.hpp` synchronized across module and companion. Missing/invalid identity fails open to genuine app behavior. Host checks do not establish live JNI availability, complete PLT coverage, socket/credential behavior, namespace operations, or Android ABI behavior.

## Rotation, AppLog, and WebUI contracts

`rotate_ids.sh all --from-identity` consumes `GOOGLE_AID`, `BOOT_COUNT`, and `APPLOG_EPOCH` from the committed snapshot. It validates the UUID/decimal inputs, XML-escapes written GAID values, and reports only SSAID, GAID, and boot-count components. Standalone local changes acquire mutation ownership and persist through native `set-local`.

Action AppLog work wipes/seeds each installed unique package with the one committed `APPLOG_EPOCH`; aggregate success may not hide a failed package. Synthetic Wi-Fi/Bluetooth identities and SIM/carrier presentation are not runtime features. Framework application still synchronizes general user-0 device-name settings from persona `MODEL`; genuine global properties are preserved for framework compatibility, while validated target-process Java property hooks hide covered direct identifiers.

The WebUI is static and privileged through `ksu.exec`. Preserve external-only CSP, `shq` quoting, and base64 treatment of target text. It provides a bounded callback timeout with global cleanup, one mutation coordinator, Action progress/run polling, durable timeout reconciliation, exit-preserving script logging, atomic target publication with validation/rollback, accessible ARIA tabs and keyboard navigation, and distinct success/reboot/partial/rollback/degraded/busy/unknown states.

## Build, package, and release

`build.sh` requires core runtime files as regular files, verifies pinned `zygisk.hpp`, builds four ABIs, verifies the complete resetprop binary/checksum/license set, runs `tests/package_manifest_test.sh`, and treats `personas.tsv` as an optional extension. It performs no persona network refresh. Missing `module.prop` remains a deliberate hard blocker.

The resetprop binary is pinned to upstream `Enginex0/resetprop-rs` v0.6.0 with documented asset/hash/source provenance and MIT notice, but no signature, SLSA attestation, SBOM, or reproducible-build proof.

`.github/workflows/build.yml` can commit metadata, tag, and publish a release after a default-branch update; it has no pull-request trigger. Do not trigger CI/release, merge a PR, tag, or publish unless explicitly authorized for that distinct action.

## Detailed ledgers

- `docs/codebase-map.md`: component/state ownership and cross-layer coupling.
- `docs/native-hooks.md`: native transaction, CLI, hooks, companion, IPC, mount/hide behavior.
- `docs/shell-lifecycle.md`: installer, boot, Action, acquisition, rotation, and helper contracts.
- `docs/webui-build-tests.md`: WebUI, packaging, validation coverage, and release boundaries.

Keep these synchronized whenever a command, state path, metadata field, protocol, marker, package input, or test contract changes. In future audits, start from git status/diff and these ledgers; reread only changed files and direct consumers unless a broader trace is necessary. Source remains authoritative.