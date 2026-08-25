# Domain glossary — read this before reviewing

SandboxID is a **root-level Android module** (KernelSU / Magisk ≥ 26100 /
APatch) that runs a **Zygisk** module pre-zygote plus a root companion, to let a
device owner study and reconfigure the identifier fields apps read on their own
device. Terms used throughout the code and these instructions:

- **Persona / identity** — a coherent set of device fields (`Build.*`,
  `ro.product.*`, serial, MACs, IDs) written to `identity.prop` and applied.
- **freshen** — native `sandboxid freshen`: pick a persona from `personas.tsv`
  (the multi-brand offline-fallback pool) and write `identity.prop`. The backup
  path when multibrand fails.
- **multibrand / device** — `autopif.sh device` draws a real-device persona
  from `devices.tsv` (Google/Samsung/Xiaomi/Poco/Vivo/Oppo/Infinix/Redmi) into
  `device.identity`, applied by the native binary. The primary `action.sh` path.
- **rotate** — `rotate_ids.sh`: regenerate the *shell-layer* IDs (SSAID, GAID,
  Wi-Fi/BT MAC, device name, boot count) to match the persona.
- **apply-boot / seed** — native subcommands that push props via `resetprop-rs`
  at boot (`service.sh`) or fast-bootstrap at `post-fs-data` respectively.
- **companion** — root-side process (`jni/companion.cpp`) that serves per-app
  identity blobs over the Zygisk socket, hot-reloads `target.txt`, and
  bind-mounts a synthetic `build.prop` tree into the target's mount namespace.
- **target.txt** — the packages the per-app layer acts on. **Ships empty** =
  module is fully idle. An empty target list is the correct no-op default, not a
  bug. Do not flag "does nothing when target.txt is empty".
- **resetprop-rs** — prebuilt binary (`prebuilt/resetprop-rs`) that sets `ro.*`
  system properties at runtime; checksummed by `prebuilt/resetprop-rs.sha256`.
- **exemptFd** — Zygisk API call to keep an fd open across specialize; on strict
  Zygisk providers it returns false on the USAP/specialize path (a known gotcha).
- **L3 / LSPlant hook** — the experimental Java-method hook (LSPlant + Dobby)
  behind the `SBX_ENABLE_LSPLANT` build flag, **OFF by default** and its
  generated `hook_dex.h` is not checked in. Per-app `ANDROID_ID` spoofing needs
  it; treat it as inactive out of the box (see README "Known limitations").

## Two layers, two scopes (do not conflate)

1. **Device-wide layer** — boot-time `resetprop` via `apply-boot`, active only
   when `target.txt` is non-empty; affects *every* process.
2. **Per-app layer** — Zygisk hooks + `build.prop` bind-mount into the target
   app's mount namespace; affects *only* listed targets.

Enabling one does not imply the other. A finding that assumes a single global
scope is usually wrong for this codebase.

## Native vs shell boundary — affects how a fix ships

- **Shell** (`*.sh`, `webroot/*`, `*.tsv`, `target.txt`): edits are **live** on
  device — the running module picks them up (script re-run / mtime watch). A fix
  here is verifiable without reflashing.
- **Native** (`jni/*.cpp`, `jni/*.hpp`, `jni/*.java`, `CMakeLists.txt`): edits
  require a **CI rebuild + reflash + reboot** to take effect. A reviewer cannot
  assume a native change was runtime-verified; on-device confirmation is a
  separate, later step. Call this out when a native diff claims to "fix" a
  runtime symptom.
