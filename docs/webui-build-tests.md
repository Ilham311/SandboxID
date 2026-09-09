# WebUI, Build, and Test Ledger

This ledger records the current static WebUI behavior, privileged bridge contract, build/package policy, safe validation coverage, and release boundary. Source is authoritative.

## WebUI structure and security

`webroot/index.html`, `style.css`, `theme-init.js`, and `app.js` are served directly by a KernelSU/APatch-compatible manager. There is no framework, dependency installation, bundler, or WebUI compilation step.

The CSP permits local external scripts/styles and blocks inline script, handlers, and styles. Preserve `shq` for scalar shell values and UTF-8/base64 transport for free-form target text. Dynamic output is escaped before insertion; log rendering never treats device output as markup.

The privileged bridge is `ksu.exec(command, '{}', callbackName)`. `exec` creates a unique global callback, applies a bounded timeout, and deletes the callback on success, error, synchronous throw, or timeout. A timeout is classified as unknown outcome—not command failure and not permission to retry. Action reconciliation reads only durable state/result matching the generated 32-lowercase-hex run.

One Promise-tail mutation coordinator serializes Action, rotation, carrier, flag, target-save, and refresh operations. Mutation controls and relevant `aria-busy` state remain blocked while work is active. Shell/native ownership remains authoritative across WebViews and processes.

Most observational reads intentionally tolerate missing files and can therefore render empty state for absence or unreadability. Mutation commands do not use that permissive policy.

## Action controller and protocol

The main CTA runs one Action without an extra confirmation dialog, while adjacent copy states that target data and device-impacting identifier stores may change and a reboot or partial result is possible.

The controller generates the run ID, starts `action.sh`, polls `debug/action.state`, and parses only records whose first tab-separated field is exactly `SBX_ACTION_V1`. It correlates state/result by run and renders stage, counters, and terminal classes independently from human-readable logs. Outcomes distinguish complete, reboot-required, committed partial, restored, degraded, busy, configuration/tool failure, and unknown.

When the bridge times out, the UI polls durable files for the matching run. A live run keeps mutation controls blocked. A matching terminal result resolves normally. If the owner is no longer live and no terminal result exists, the UI warns that the result is unknown and never automatically reruns Action.

## Navigation, rendering, and accessibility

Tabs use `tablist`, `tab`, and `tabpanel` roles with IDs/relationships, `aria-selected`, roving `tabIndex`, hidden inactive panels, and ArrowLeft/ArrowRight/Home/End navigation. Initial and tab-specific loaders are awaited to avoid overlapping stale renders. Action progress uses live semantics; skeletons are hidden from assistive technology when inactive.

Toasts and logs escape payloads and expose textual status rather than color alone. Focus rings, labels, safe-area layout, responsive grids, and reduced-motion behavior are preserved. `parseProp` remains a permissive display parser and is never a substitute for native identity validation.

## Tab-to-runtime map

### Perangkat

Reads canonical identity for display and runs the one-click Action. It shows measured canonical/target/runtime state rather than claiming that every hook is active. Refresh preference is explicit and does not make network acquisition mandatory.

### Rotasi

Standalone cards invoke supported `rotate_ids.sh` commands through the global mutation queue. Commands capture combined output and the original exit code, append output to `debug/rotate.log`, then exit with that original code; no `tee` pipeline can mask script failure. Rendering uses explicit component/report status, warnings, and reboot requirement. Reloads are awaited.

### SIM

`carriers.tsv` is optional convenience data. Parsed catalog rows are validated before use. When absent, manual MCC, MNC, name, ISO, phantom, and optional carrier-ID fields remain available with client-side format checks; native carrier parsing/transaction is final authority. Apply and disable preserve the original shell exit status while appending logs.

### Eksperimen

Six operational flags are changed through native `set-flag` and canonical state is reloaded after mutation. Copy states the partial cross-lens scope. Marker files and native policy remain authoritative for hide/native-read effective behavior.

### Target

Load displays `target.txt`. Save normalizes line endings, base64-encodes the complete text, writes a same-directory private temporary file, validates it with native target parsing, applies mode, renames it atomically, and restores the previous file if publication verification fails. Deliberate empty content is valid and preserved as the off state. Exact process names remain distinct; base-package normalization is native behavior.

### Uji and Log

Self-test parses only exact `SELFTEST` records and states that root-shell diagnostics cannot prove target-process activation. Log views read bounded device evidence and escape it. Debug initialization does not redefine Action success.

## Native build and package policy

`jni/CMakeLists.txt` builds `libsandboxid.so` and the privileged CLI with C++20; optional host targets test header-pure behavior. A default host build is not an Android build.

`build.sh` requires the NDK path, CMake, zip, `module.prop`, mandatory runtime/WebUI inputs, and SHA-256 tooling. It fetches only missing pinned `jni/zygisk.hpp` from commit `8ce26128f81baaed0b969aaf7f52f886b61af4ab` and requires SHA-256 `f8d55e8b4f89d418c5941afe62ce6a09ddec1f4afd9a1b0a01eb40a93310dd28`. No persona refresh or network persona source runs during packaging.

Release/debug variants build arm64-v8a, armeabi-v7a, x86_64, and x86 at minimum API 26 by default. Core lifecycle scripts, target file, WebUI, and four CLI binaries are mandatory. `personas.tsv` and `carriers.tsv` are optional. `tests/package_manifest_test.sh` verifies staged inputs and rejects obsolete acquisition artifacts. `dist` is not cleaned, so stale ZIPs can coexist with new output.

Missing `module.prop` remains a deliberate full-build blocker. Missing generated `jni/zygisk.hpp` remains a `main.cpp` syntax blocker during non-network validation.

## Bundled resetprop

The optional bundled arm64 binary/checksum/license form one indivisible package unit. The binary is pinned to `Enginex0/resetprop-rs` v0.6.0 with SHA-256 `6d82fc8e92089ce24226636ec4d24deb15655d7a7473d37d24fa4ae53588c1df` and an MIT license copy. Build refuses incomplete or unverifiable sets; installer removes them together when wrong-ABI, incomplete, or unverifiable. Provenance does not establish signature, SLSA attestation, SBOM, or reproducible-build equivalence.

## Safe validation map

`bash validate.sh` runs eight aggregate stages:

1. strict debug/release syntax for native files available without Zygisk, and `main.cpp` only when the generated header exists;
2. tracked shell and JavaScript syntax;
3. carrier, native-read, persona, and target C++ host suites plus Python probe policy;
4. mocked Action, acquisition, rotation, AppLog/helper, installer, package-manifest, and dependency-free WebUI suites;
5. acquisition adapter/no-redirect policy;
6. explicit package blockers and obsolete-source policy;
7. `git diff --check`;
8. ShellCheck when installed.

`tests/webui_test.js` covers protocol parsing, callback timeout cleanup, unknown-result reconciliation, mutation serialization, exit-preserving logging, awaited reloads, target-save construction, and tab behavior. Static assertions cover CSP/ARIA contracts. Package tests cover mandatory inputs, optional catalogs, resetprop completeness, and obsolete-file exclusion.

Host/mocked success establishes code-level parsers, state machines, generated commands, ownership, accounting, and UI behavior. It does not prove a real root-manager callback ABI, WebView/CSP implementation, Android property/settings operations, SELinux ownership, OEM identifier layouts, package-data mutation, Wi-Fi/Bluetooth acceptance, Zygisk/JNI/PLT coverage, namespaces, or four-ABI execution.

## CI and release boundary

The tracked release workflow runs for default-branch pushes, version tags, and manual dispatch—not pull requests. It can rewrite/commit metadata, tag, and publish a GitHub release. Metadata-push and source/artifact consistency caveats remain documented in the workflow itself; safe local validation does not trigger it.

Opening a feature-branch pull request is distinct from merging it. Do not merge, tag, dispatch the workflow, build, or publish without separate authorization. No build, device-impacting command, live refresh, CI run, tag, release, or publication was performed to create this ledger.