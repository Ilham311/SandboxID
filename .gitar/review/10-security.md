# Security review — root module, companion IPC, mounts, SELinux

This module runs as **root**, pre-zygote, and injects into app processes. The
security review is about **trust boundaries** and **fail-safe direction**, not
generic OWASP web items. Confirm the invariants below hold; a change that
weakens one is **Critical**.

## Trust boundaries

1. **WebUI → root shell** via `ksu.exec` — the top injection surface. Reviewed
   in `40-webui-and-parse-contract.md` (every interpolated value must pass
   `shq()` or the base64 pattern). A raw `${var}` reaching the bridge is
   Critical.
2. **App process → companion** over the Zygisk Unix socket. The companion is
   root; an app is not. Authorization must stay **asymmetric** (below).
3. **Network → device**: the *only* outbound path is `autopif.sh` best-effort
   fetch of Pixel fingerprints from Google, a **no-op offline**. Flag **any new
   outbound connection**, telemetry, or URL as Important/Critical — this module
   is local-only by design and MIT/"transparent".

## Companion IPC invariants (`jni/companion.cpp`, `jni/config.hpp`)

- **`CMD_GET_IDENTITY` is fail-open on purpose**: a non-target or empty identity
  → the app simply runs **unspoofed** (`companion.cpp:292-311`). Do not "harden"
  this into fail-closed — it would break the ships-idle guarantee. Not a finding.
- **`CMD_DO_MOUNTS` is fail-closed and must stay so**: it is authorized only
  when `SO_PEERCRED` reports `uid == 0 && pid == caller` (`companion.cpp:269-276,
  353-362`). Any change that mounts without re-verifying peer creds, trusts a
  client-supplied pid/uid, or widens this to non-root is **Critical** (a
  malicious app could drive a root bind-mount into its own namespace).
- **Bounded reads**: the identity blob is capped at `MAX_IDENTITY_BLOB = 64 KiB`
  (`config.hpp:35`, enforced `main.cpp:620`) and framed via `read_full`/
  `write_full`. Removing the cap or reading an attacker-supplied length without
  a bound is a memory-safety / DoS finding.
- **Socket lifetime**: the companion fd is kept across specialize via
  `exemptFd` (`main.cpp:607`) with a 2 s send/recv timeout. Don't drop the
  timeout (hang risk) or assume `exemptFd` succeeded — on strict Zygisk it can
  fail (see `20-native-and-hooks.md`); the path must fail safe.

## Mount / namespace safety

- Mounts run in a **forked child that `setns()` into the target's mount ns**
  (`do_mounts_via_fork`, `companion.cpp:113-213`), and the child does
  `mount("", "/", MS_SLAVE | MS_REC)` **before** any `MS_BIND`. That propagation
  containment must be preserved — a bind mount made without first making the
  mount tree slave/private can leak into other namespaces (**Critical**).
- Bind sources are opened as fds and mounted from `/proc/self/fd/N` onto the
  entries in `BIND_ENTRIES` (`config.hpp:39-51`). New bind targets must stay
  inside the intended `build.prop`/settings set — no path outside it.

## Filesystem, secrets, supply chain

- **Atomic writes**: `identity.prop` / `module.prop` are updated via
  `helpers.sh identity_persist` (awk + rename) — atomic upsert. A non-atomic
  in-place rewrite that can be interrupted mid-boot is a corruption finding.
- **Backups before destructive ops**: the README promises a backup before
  wiping `WifiConfigStore.xml` and before rotating SSAID. Deleting a settings
  XML without the documented backup is Important/Critical.
- **`prebuilt/resetprop-rs` is checksum-gated** (`prebuilt/resetprop-rs.sha256`,
  verified in `build.sh`/`customize.sh`). Never bypass or weaken the checksum —
  it is the supply-chain guard for a root binary.
- **Permissions**: module files are written `0644` (dirs `0700` for debug
  artifacts). Flag any `chmod 0777`, group/other-writable file under
  `/data/adb/modules/sandboxid`, or a debug/report path that leaks identity data
  world-readable.
- **SELinux**: the scoped `se_permissive`/`se_restore` pair is ref-counted and
  restored by an `EXIT INT TERM HUP` trap (`helpers.sh:36-53`). Never propose a
  persistent `setenforce 0`, never remove the trap/`se_restore`, and don't widen
  the permissive window beyond the single file rewrite it guards.

## Not findings (documented / by design)

- Requiring root, injecting into apps the owner listed, and spoofing identifiers
  are the module's *purpose* on the owner's device — not vulnerabilities of the
  module against itself.
- The honest gaps in README "Known limitations" (per-app `ANDROID_ID` needs the
  disabled L3 hook; `SystemProperties.find()` fast path unhooked; mixed identity
  for non-targets; `/proc/uptime` not spoofed) are known and intentional.
