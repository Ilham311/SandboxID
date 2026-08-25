# Severity rubric — calibrated for a root-level Zygisk module

Map every finding to Gitar's three severities using **this** project's blast
radius, not generic web-app intuition. This module runs as root, pre-zygote, in
and around `system_server`; the failure modes are device-level, not
request-level.

## Critical — block merge; fix before ship

- Anything that can **bootloop** the device or crash `system_server`: a native
  hook with a wrong JNI signature, a null deref on the specialize path, an
  unconditional `abort()`/`LOG(FATAL)`, a `service.sh`/`post-fs-data.sh` change
  that can hang or fail the boot stage.
- **Identity leak that defeats the module's purpose**: the *real* device value
  reaching a target app when a persona is active (e.g. a hook that no longer
  covers a read path, a prop set on the wrong partition, an un-spoofed field the
  persona claims to set).
- **Privilege / injection**: unsanitized input reaching a root shell
  (`ksu.exec`, `system()`, `sh -c`, `eval`), a world-writable path under
  `/data/adb`, a bind-mount escaping its intended target, an fd/SELinux domain
  leak from the companion into an app.
- **Corruption of persisted state** that can brick the module: a non-atomic
  write to `identity.prop`/`module.prop`, a bad `resetprop` on `ro.*`, deleting
  a settings XML without backup where the README promises one.
- A parse-token change that flips a **failure into a reported success** in the
  WebUI (user believes the persona applied when it did not).

## Important — should fix; may merge with a tracked follow-up

- **Silent WebUI breakage** from the parse-token contract (see
  `@../documents/parse-token-safelist.md`): a renamed field, dropped tile, or
  mis-classified log line — visible wrong state, but not a safety issue.
- Missing kill-switch / fail-safe on a risky native feature (a runtime toggle
  that no longer disables the code path it guards).
- Behaviour that diverges from README's documented contract (CLI verbs, schema
  columns, "ships idle" guarantee) without updating the docs.
- Resource / robustness bugs that degrade but don't brick: leaked fd, unbounded
  log growth, missing `2>/dev/null` on a noisy path that spams logcat, a
  `pm`/`am`/`cmd` call without `--user` or without `/dev/null` std FDs.
- Portability breaks across the supported matrix (Android 13–16, KernelSU /
  Magisk / APatch, the listed Zygisk providers, all four ABIs).

## Suggestion — nice to have; author's discretion

- Readability, naming, dead code, duplicated logic, comment drift.
- Micro-optimizations with no measured impact.
- Style nits **except** the house-style rules in
  `50-output-and-localization.md`, which are Important when they touch
  user-facing copy (they are a product decision here, not taste).

## Verdict guidance

Prefer **fewer, higher-confidence findings**. This is a small,
carefully-reasoned codebase (see the "Known limitations" section of the README —
the authors already document the honest gaps). Do **not** re-report a documented
limitation as a bug: per-app `ANDROID_ID` needing the disabled L3 hook, the
`SystemProperties.find()` fast path being unhooked, non-target apps seeing a
mixed identity, and `/proc/uptime` not being spoofed are all **known and
intentional**, not findings.
