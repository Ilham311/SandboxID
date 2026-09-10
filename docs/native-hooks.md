# Native Hooks and IPC Ledger

This is the method-level native reference for the one-click identity architecture. Source remains authoritative. Generated `jni/zygisk.hpp` is absent in this checkout, so live Zygisk ABI behavior is an explicit integration boundary.

## Products and shared headers

`jni/CMakeLists.txt` uses C++20 and builds `libsandboxid.so` from `main.cpp` plus `companion.cpp`, and the privileged `sandboxid` CLI from `sandboxid.cpp`. Optional host targets exercise header-pure behavior; a default host build must not be treated as an Android build.

| Header | Contract |
|---|---|
| `config.hpp` | Paths, 64 KiB identity cap, overlay/bind tables, IPC command numbers, exact full-read/full-write helpers. |
| `sbx_identity.hpp` | Strict identity parser, cross-field coherence, local-ID validation, operational flags, canonical serialization. |
| `sbx_persona.hpp` | Bounded persona rows, exact-SDK selection, source tiers, compiled reviewed SDK 31–36 catalog. |
| `sbx_target.hpp` | Exact process syntax, comment handling, stable deduplication, normalized base packages. |
| `sbx_transaction.hpp` | Canonical/pending metadata, owner records, provenance, run/SHA validation, state digest. |
| `sbx_sha256.hpp` | Header-pure SHA-256 used for all content bindings. |
| `sbx_property.hpp` | Mapped/hidden/pass decisions and typed/callback helpers. |
| `sbx_native_read.hpp` | Read classification/transforms and deterministic AppLog IDs. |
| `sbx_mountinfo.hpp` | Protected mount recognition and trace-detach selection. |

## Identity, persona, and metadata contracts

Identity input is bounded, line-oriented, unique-key, control-character checked, and validated against the genuine runtime SDK. Required build/lifecycle relationships include canonical fingerprint, description, flavor, display/build ID, UTC build date, and paired SoC fields. `ANDROID_ID` is 16 lowercase hex; GAID is UUID-v4 or the all-zero sentinel; boot count and AppLog epoch are decimal. The five operational flags accept only `0|1`.

Capability, ABI, preview, verified-boot, and other service/hardware-owned legacy keys are not promoted into the presentation identity. Migration may drop only the explicit legacy/retired sets; app activation remains strict. Retired state includes synthetic Wi-Fi/Bluetooth identities, sysfs-MAC presentation, and SIM/carrier keys. Canonical serialization cannot emit those keys.

Persona priority is: valid exact-SDK override, persistent validated cache, reviewed extension row, then compiled reviewed row. Invalid optional tiers warn and fall through. Compiled rows cover SDK 31–36; an unknown SDK fails before mutation instead of fabricating metadata. Override removal occurs only after successful commit. Cache replacement requires exact-SDK candidate parsing, full derived-identity validation, and matching acquisition provenance.

Canonical metadata is `version=1`, a 32-hex run, `source=override|cache|extension|builtin`, and the identity SHA-256. Pending metadata additionally binds the canonical identity/metadata pair through `base_state_sha256`. Legacy three-field metadata is accepted only for narrowly scoped migration and is never a pending transaction.

## Durable native state

The CLI is the sole canonical writer. `.state.lock` is an exclusive `flock` for mutations and a nonblocking shared lock for companion reads. Mutation commands also validate `.mutation.lock/owner`: owner metadata binds version, kind, PID, `/proc` start ticks, token, and Action run where applicable; the invoking process must descend from the owner.

Required writes check open, complete write, mode, file `fsync`, close, same-directory rename, directory open, and directory `fsync`. Pair replacement and overlay publication restore their previous state on the executing return path when possible. This is not a journal: power loss or SIGKILL between multi-file renames can still require later recovery.

Overlays are generated as one checked five-part set (`system`, `vendor`, `odm`, `product`, `system_ext`), staged, mode-checked, activated, and verified against canonical identity. Commit backs up the prior validated canonical pair before activation. A failed activated overlay/canonical update attempts coherent restoration and returns degraded status when that cannot be proven.

## CLI command semantics

| Command | Native behavior |
|---|---|
| `prepare [run-id]` | Reject locked mode; select exact-SDK persona; preserve validated operational flags; generate remaining local values once; validate; publish pending pair bound to current base. No property, package, store, or AppLog mutation. |
| `commit <run-id>` | Revalidate pending run/hash/identity and unchanged base; build/stage overlays; preserve old canonical pair; activate overlays and canonical pair; consume override only after success; print committed digest. |
| `abort <run-id>` | Remove only a matching valid pending pair. |
| `restore` | Validate backup pair, rebuild overlays, and restore canonical state; does not clear targets or reapply properties/settings. |
| `verify [--run-id …] [--identity-sha256 …]` | Shared-lock, strict-validate canonical pair, optional exact run/digest, and exact overlay contents/modes. |
| `persona-import <candidate> <meta>` | Validate one canonical candidate plus HTTPS provenance and atomically replace the cache pair while preserving old cache on failure. |
| `targets --processes|--packages` | Return exact deduplicated processes or normalized unique base packages from one parser. |
| `set-local`, `set-flag` | Validate owner, remaining supported key/value, whole identity, metadata, and overlays before coherent publication. |
| `action-write state|result <source>` | Validate the bounded Action document/protocol and durably publish it under `debug/`. |
| `apply-props` | Strict-load canonical identity and apply global property aliases/removals through the selected backend. |
| `apply-boot` | Apply framework-ready settings and regenerate checked overlays. |
| `seed` | Keep a valid bound canonical pair, migrate the narrow legacy form, or create state only when genuinely missing. |
| `applog-ids <package>` | Derive deterministic IDs from canonical persona, package, and committed AppLog epoch. |
| `freshen` | Compatibility wrapper over prepare/commit/apply; no implicit target wipe. |
| `rollback` | Deprecated alias for identity restore; target clearing remains separate. |

Exit values distinguish ordinary failure, usage/configuration (`64`), busy ownership (`75`), committed cleanup partial (`20`), and degraded persistence (`32`) where those classes apply. `status` is observational and does not establish target-process activation.

## Property and framework behavior

Native property application prefers the verified bundled `resetprop-rs`, then supported PATH backends according to implementation policy. Required aliases are attempted and any backend failure is reported. The explicitly classified OEM alias `ro.build.expect.baseband` is existing-only: genuine absence is a successful skip, while a failed write when present remains fatal. `gsm.version.baseband` remains required, and both radio aliases remain represented in build-property overlays. Presentation aliases cover build/product/serial/radio metadata while selected direct-ID, emulator, custom-ROM, and OEM leak properties—including genuine telephony, Wi-Fi, and Bluetooth identifiers—are removed rather than invented. Stable-release aliases are changed only when the genuine runtime is stable. Framework application writes user-0 device-name settings from persona `MODEL`; this general setting is distinct from retired synthetic Bluetooth-name state and does not pretend `ANDROID_ID` is one global secure value.

## Companion target and identity service

The target cache reads one FD with before/after `fstat` and publishes only a stable parse. Stable empty content becomes the authoritative off state. A transient missing, unreadable, or racing reload retains the last stable set. Matching is exact against Zygisk `nice_name`; every secondary process must be listed explicitly.

For a targeted GET, the companion takes a nonblocking shared state lock, reads identity and metadata together, checks canonical metadata and SHA binding, and returns `Ready`, `Missing`, `Busy`, or `Invalid`. Busy is retried briefly. Seed is attempted only for genuinely missing state; invalid state is never replaced as a side effect of a GET. Any non-ready outcome returns no identity so the app remains genuine.

Memory-only marker adjustments occur after bound validation: `no_uptime` zeros existing uptime fields, `no_native_read` forces the delivered native-read master off, and `enable_hide` forces delivered hide on. The companion independently requires `enable_hide` again before executing hide.

## IPC and Zygisk lifecycle

IPC IDs remain `GET_IDENTITY=2`, `DO_MOUNTS=3`, and `DO_HIDE=4`, with local integer framing. GET sends a 16-bit process-name length and receives a 32-bit blob length. Mount/hide sends a 32-bit PID and receives a 32-bit successful-operation count. Exact I/O retries `EINTR`; there is no wire version or cross-endian conversion.

`preAppSpecialize` converts the exact process name, opens the companion FD, requests identity, enforces the 64 KiB cap, obtains the genuine runtime SDK, and strict-validates before marking the module active. Non-target and rejected responses close the socket without requesting an exemption. Only a validated target attempts to exempt the FD for post-specialization IPC; exemption failure closes it before specialization, keeps in-process presentation active, and disables build-property mounts and optional hide for that process. Zero/invalid/unavailable data unloads the module and leaves the process genuine. `preServerSpecialize` always unloads.

`postAppSpecialize` publishes the validated snapshot, determines genuine stable-release state, mutates selected Java `Build`/`VERSION` fields, installs Java string/handle/typed property hooks, installs uptime, sets the AppLog package seed, installs Bionic property/native-read PLT hooks, and arms the crash watchdog. When pre-specialization FD exemption succeeded, it requests build-property mounts, optionally requests hide, and closes the companion socket. Individual hook/mount failures do not abort app startup.

## Hook lenses and limits

Java string getters can map or hide by key. Handle APIs first need a genuine handle and therefore cannot invent an absent property. Bionic name-based `__system_property_get` can synthesize mapped absent keys; read/callback forms require a genuine property/read and preserve callback cookie/name/serial. Native capability-property groups pass through. Java hooks remain active when the native-read master is off; the Bionic PLT property hooks share that master.

Selected Java `Build` fields and stable-release `Build.VERSION` fields are changed; SDK, ABI arrays, capabilities, board/hardware/SoC, preview state, and attestation remain genuine unless another explicitly documented lens covers them.

The uptime hook offsets only `CLOCK_BOOTTIME` and `CLOCK_BOOTTIME_ALARM`. Incomplete registration/commit resets the offset. It covers only currently scanned imports.

Native read hooks classify read-only `open`/`openat`/`fopen` families and may serve memfd content for boot ID, SELinux enforce, allowlisted AppLog state, and opt-in proc surfaces. AppLog IDs are deterministic from fingerprint, serial, Android ID, package, and one committed epoch. Proc version, meminfo, and CPU aggregate revision require their child flags. WLAN/P2P sysfs address paths are not classified or synthesized. Classification is lexical rather than symlink-canonical; descriptor flags and late-loaded-library coverage are not complete. Transform failure normally delegates to genuine reads.

PLT scanning covers currently mapped libraries and active status does not prove every symbol/import was hooked. Host tests cannot prove JNI signatures, field availability, PLT registration, callbacks, late loads, or manager-specific Zygisk behavior.

## Mount, hide, and crash behavior

Mount/hide requests require `SO_PEERCRED`, root UID, and requested PID equal to the peer PID. Children fork before `setns`. Mounts require successful recursive `MS_SLAVE`; missing source/destination is a skip, bind failures are logged, and the module receives only the success count. Hide selects unprotected root traces from mountinfo and detaches with `MNT_DETACH`; it currently warns but continues if propagation isolation fails, so that condition remains a device-integration risk. Required overlay sources/destinations and system roots are protected.

The target-death watcher is informational and bounded. The crash watchdog handles selected fatal signals through a nonblocking pipe and chains prior/default behavior; it is diagnostics, not recovery.

## Verification boundary

Header/mocked tests establish parser, derivation, ownership, transaction-return, target, property, native-read, AppLog, and mount-selection contracts. They do not establish real Android property/settings behavior, SELinux labels, namespace isolation, mount propagation, GMS/OEM stores, signal chaining, companion socket deadlines, Zygisk/JNI/PLT coverage, or ABI package execution. Missing `jni/zygisk.hpp` remains an explicit `main.cpp` syntax blocker.