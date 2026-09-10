# Shell Lifecycle and State Ledger

This ledger covers installer, boot, Action, acquisition, rotation, helper, diagnostic, build, and validation behavior. Source is authoritative. None of the device-impacting paths described here are safe substitutes for mocked/host validation.

## End-to-end lifecycle

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
```

`target.txt` being stably empty is the intentional off state for automatic boot work and Zygisk targeting. Explicit standalone rotation remains device-impacting and is not made safe by an empty target file.

## Installer: `customize.sh`

The installer describes the one-run effects and presentation boundary, then preserves an existing readable `target.txt` even when empty, `identity.mode`, validated canonical/backup pairs, and the validated persona-cache pair. It deliberately does not preserve retired carrier state/catalogs or the retired sysfs-MAC flag. Copy failures abort rather than being reported as success. A narrowly scoped metadata-less legacy identity may be preserved for native migration; malformed or incomplete modern pairs are rejected.

Operational flags are extracted only after identity preservation and saved for native restoration. Mandatory scripts and all four ABI CLI binaries must exist. Unsupported ABI aborts instead of installing without a CLI. The selected binary is linked as `bin/sandboxid` and verified executable.

Bundled resetprop files are one unit: regular binary, checksum, and license. On arm64 they require `sha256sum` verification; incomplete, unverifiable, mismatched, or wrong-ABI bundles are removed together, leaving a documented PATH-backend requirement. The installer creates private backup/state permissions, WebUI permissions, and the five overlay roots. It does not recreate deleted historical catalogs or `module.prop`.

## Boot phases

`post-fs-data.sh` resolves the ABI CLI, restores accepted operational flags through native `set-flag`, removes the transient flag file after attempts, and exits early for an effectively empty target file. Otherwise it runs `seed` and only then `apply-props`; seed failure prevents property application. Its log falls back from `/cache` to the module directory when needed. If no executable CLI resolves, guarded work is skipped, which remains an integration failure boundary.

`service.sh` waits without a deadline for `sys.boot_completed=1`, pauses five seconds, and runs `apply-boot` only when targets are effective, canonical identity exists, and the CLI is executable. Its apply group still redirects to `/cache/sandboxid-boot.log`; failure to open that path can prevent execution while leaving the initialized result unchanged. Debug variants prune old logs and start logcat/journal workers, but PID replacement does not prove older workers were terminated.

## Action ownership and protocol

`action.sh` acquires both `.action.lock` and `.mutation.lock`. Owner records bind version, kind, PID, `/proc` start ticks, 32-hex token, and Action run. A live owner returns `75`; only demonstrably stale directories are renamed and removed. Descendant scripts inherit and revalidate the same owner. Traps release only locks still matching this run/PID/start/token.

Action writes bounded state/result documents through native `action-write`. Machine output is tab-delimited `SBX_ACTION_V1`; Indonesian logs are supplementary. State carries run, stage, timestamps, counters, and terminal class. Exactly one matching terminal result supports WebUI timeout reconciliation.

Stages and mutation policy:

1. **preflight** validates root/tools, ownership, unlocked mode, effective exact targets, normalized packages, property backend, runtime persona support, package inventory, and writable state/debug locations. Package inventory first uses bounded retries for user-scoped and compatibility global Package Manager queries. If both inventories remain unavailable, each target is checked through the independent `dumpsys package <target>` route: exactly one matching package block with one unambiguous user-0 `installed=true|false` state, or one exact not-found marker, is required. Missing, denied, malformed, mismatched, or conflicting dump output remains unknown and falls through to bounded exact package-name-filtered Package Manager queries. Failed command output and bounded dump diagnostics are retained in `debug/action.log`; successful full dumps are not logged. Only explicit evidence from a successful route can establish installed or absent state, and any unresolved target stops preflight before mutation.
2. **refresh** always invokes bounded `autopif.sh refresh` after package classification; failure emits one warning and offline persona selection remains available.
3. **prepare** asks native code to create and validate pending identity bound to current canonical state; it performs no device/app mutation.
4. **commit** publishes checked overlays and canonical identity/metadata for the exact run.
5. **apply** runs native property and framework application. Required property aliases remain fatal on backend failure. The explicitly classified OEM alias `ro.build.expect.baseband` is existing-only: genuine absence is a successful skip, but a failed attempted write remains fatal. Any fatal apply failure before irreversible work invokes restore, reapply, and exact verification.
6. **targets** attempts force-stop and independently attempts `pm clear` for every installed unique base package; absent packages are explicit skips.
7. **rotate** invokes `rotate_ids.sh all --from-identity` and consumes its run-bound component report.
8. **applog** wipes/seeds every installed unique package with the one committed epoch.
9. **verify** checks the exact run, identity digest, metadata, and overlays, then aggregates all package/component outcomes.
10. **result** durably records success, reboot, partial, rollback, degraded, configuration, or busy outcome.

No target data, identifier store, or AppLog state is touched after preflight/prepare/commit/initial-apply failure. Once any irreversible package/store mutation begins, Action keeps the new canonical identity and reports partial forward recovery instead of restoring a mismatched old world. Exit classes are `0`, `10`, `20`, `30`, `31`, `32`, `64`, `75`, and `127` as defined in `CLAUDE.md`.

## Pixel OTA acquisition: `autopif.sh`

The only public command is `refresh`, and every otherwise-valid Action calls it after package classification and before native `prepare`. SDK 35 and 36 map to official exact-release Android 15 and 16 catalogs; older supported runtime SDKs fail acquisition before networking because their former version URLs redirect to a generic page, then Action uses offline selection. The adapter uses an explicit `developer.android.com`/`dl.google.com` HTTPS allowlist, bounded connection/overall time and response size, an explicit bounded OTA byte range, and no implicit redirects. Temporary data is private and contains no canonical or local identifiers.

The `pixel-ota-v1` adapter joins supported Pixel model rows to direct OTA URLs by device name, randomizes bounded traversal, and treats the device from a hash-bound prior cache as an exclusion constraint. If no supported alternative remains, refresh returns nonzero without publication; it does not immediately repeat the prior remote device. From the OTA prefix it requires authoritative `post-build` and `post-security-patch-level` values, an exact runtime release, matching `<device>_beta` product, and a pinned Pixel platform/SoC mapping. Unknown devices and incomplete or inconsistent metadata are rejected rather than guessed.

The adapter emits one complete 16-column persona and provenance containing schema/parser versions, positive retrieval time, exact runtime SDK, candidate SHA-256, adapter, and selected official OTA URL. Native `sandboxid persona-import` remains the only cache publisher and repeats strict schema/SDK/provenance validation. Network, TLS, HTTP, size, catalog, metadata, mapping, hash, native validation, or publication failure leaves the prior cache, override, and canonical identity untouched; Action warns and continues with cache/extension/compiled offline selection. A different remote device is guaranteed only after a successful refresh when a complete supported alternative exists.

## Shared helpers and mutation ownership

`helpers.sh` parses owner files once, validates decimal/PID/start/token fields, checks owner liveness and bounded process ancestry, and supports stale-lock recovery by rename-before-removal. Standalone mutations acquire `kind=standalone`; Action descendants must match `kind=action` and the Action run.

Canonical shell editing is disabled. `identity_persist` delegates `GOOGLE_AID` and `BOOT_COUNT` to native `set-local` and five operational flags to `set-flag`; unsupported keys return usage failure. Property/framework helpers retain platform-specific fallback and retry behavior, but do not become canonical writers.

SELinux temporary permissive sections are nesting-aware and cleanup traps unwind all held levels. Package enumeration comes from native `targets --packages`, so exact-process syntax, comment handling, normalization, and deduplication remain shared with native/companion behavior.

AppLog operations accept normalized packages and a supplied committed epoch. Wipe, seed, and probe report each package; aggregate accounting cannot convert a later failure into success because another package worked. Disk seed requires ownership/file checks. In-process synthesis is possible only for an actually targeted, hooked process with effective native-read, so disk failure is never described as universally harmless.

## Rotation: `rotate_ids.sh`

Action mode requires a valid inherited Action owner and `all --from-identity`. It strictly loads `GOOGLE_AID`, `BOOT_COUNT`, and `APPLOG_EPOCH` from canonical identity before mutating stores. Invalid/incomplete snapshots stop all rotation work. It never rewrites canonical local IDs in snapshot mode.

Standalone commands acquire their own mutation owner and publish desired supported local values through native `set-local` before external mutation. GAID accepts UUID-v4 or the all-zero opt-out sentinel and XML content is escaped. Boot count and AppLog epoch are bounded decimal values.

Each remaining component records `ok:<rc>`, `failed:<rc>`, or `unsupported:<rc>` in `debug/rotation.<run>` for Action. SSAID requests reboot only after verified deletion. GAID replacement restores the backed-up primary XML when later publication/ownership/context work fails. Ownership, context, and store-restoration errors contribute to status instead of being silently converted to success. Cleanup restores SELinux and releases only owned standalone locks.

Synthetic Wi-Fi/Bluetooth address and Bluetooth-name rotation, physical adapter mutation, and SIM/carrier commands are retired. Native framework apply still synchronizes general `device_name` settings from persona `MODEL`; this is not a standalone Bluetooth identity.

## Diagnostics and supporting scripts

`selftest.sh` emits exact `SELFTEST` rows and summary counts. It is observational and cannot prove target-process hook, mount, hide, JNI, or PLT activation. `summarize.sh` condenses device logs; those logs are evidence, not source. `service.sh` debug workers and their global logcat clearing remain device-side diagnostics, not safe validation.

`build.sh` requires core runtime files, the pinned verified Zygisk header, four ABI outputs, and a complete verified resetprop set when bundled. It runs the package-manifest smoke test and performs no persona acquisition. `module.prop` is a hard blocker. Optional `personas.tsv` is copied only when present.

`validate.sh` is the authoritative safe aggregate gate: native syntax where available, shell/JavaScript syntax, C++/Python host suites, mocked Action/acquisition/rotation/AppLog/installer/WebUI tests, acquisition/package policy, whitespace, and optional ShellCheck. It does not execute real Action/rotation, remote refresh, Android store mutation, build, CI, tag, release, or publication.

## Verification boundary

Mocked and host tests establish parsing, ownership, orchestration, return/accounting, rollback-on-return, and generated-command contracts. They do not prove Android property/settings behavior, OEM storage layouts, SELinux labels, GMS/SSAID behavior, namespace isolation, Zygisk hooks, four-ABI execution, or power-loss recovery.