# SandboxID — code review context & priorities

> Entry point for Gitar's custom review of this repository. Read this first, then
> the topic files (`10`–`50`). The shared reference docs below are pulled in via
> Gitar's `@` include syntax.

@../documents/glossary.md

@../documents/severity-rubric.md

## What this repo is (one paragraph)

An open-source **Android privacy-research module** that runs a **Zygisk** module
pre-zygote plus a root **companion**, letting the device owner study and
reconfigure the `Build.*` / `SystemProperties` identifier fields apps read on
**their own** device. Three cooperating layers: (1) pre-zygote property config,
(2) a per-app companion that serves identity blobs and bind-mounts a synthetic
`build.prop`, (3) a native `sandboxid` CLI + shell scripts that generate the
persona and rotate shell-layer IDs. It ships **idle** (`target.txt` empty) and
is MIT-licensed for research/education on a device you own.

## Review north star

Judge every change against these, in order:

1. **Device safety.** Does it risk a bootloop, a `system_server` crash, or a
   boot-stage hang? Native and boot-stage code is guilty until proven safe.
2. **Correctness of the illusion.** A persona must be *coherent and complete* —
   no real value leaking through, no field the persona claims but doesn't set,
   no disagreement between the device-wide layer and the per-app layer for a
   listed target.
3. **The shell↔WebUI contract.** Output strings and prop keys are an API
   consumed by `webroot/app.js`. See `40-webui-and-parse-contract.md`.
4. **User sovereignty & idle-by-default.** The module must do nothing until the
   owner opts in. Never flag the empty-`target.txt` no-op as a bug.
5. **Portability.** Android 13–16 (API 33+), KernelSU / Magisk ≥ 26100 /
   APatch, Zygisk providers (ZygiskNext, HMA-OSS, ReZygisk, NeoZygisk, built-in),
   ABIs arm64-v8a / armeabi-v7a / x86_64 / x86.

## How to write findings for this repo

- **Reproduce or refute before reporting.** State the concrete input/state and
  the resulting wrong behaviour (crash, leaked value, broken parse). If you
  cannot construct a failing path, downgrade to a Suggestion or drop it.
- **Respect the documented threat model.** The README's "Known limitations" list
  the honest gaps *on purpose*. Do not re-file them (see the rubric's Verdict
  guidance).
- **Name the layer and the ship-path.** Say whether a finding is in the native
  layer (needs CI rebuild + reflash to fix) or the shell/WebUI layer (live), so
  the author knows what verification a fix still needs.
- **Prefer signal over volume.** A handful of Critical/Important findings that
  are unquestionably real beats a long list of style nits. Group trivia.
- **Security posture is fixed, not up for debate.** The module drops SELinux to
  permissive **only** inside scoped, ref-counted, trap-restored windows
  (`helpers.sh` `se_permissive`/`se_restore`) around specific `/data` rewrites —
  never suggest a *blanket* or persistent `setenforce 0`, and never remove the
  `se_restore`/trap that guarantees it flips back (see `10-security.md`).
  Root-required-by-design behaviour is not a vulnerability to report against
  this module itself.

## Topic files

- `10-security.md` — root/privilege, injection, mounts, SELinux, secrets.
- `20-native-and-hooks.md` — Zygisk / JNI / hooks / clock virtualization / boot.
- `30-shell-scripts.md` — POSIX sh, framework CLIs, atomic writes, kill-switches.
- `40-webui-and-parse-contract.md` — WebUI (CSP/XSS/KSU bridge) + parse tokens.
- `50-output-and-localization.md` — Indonesian house style + parse-safe copy.
