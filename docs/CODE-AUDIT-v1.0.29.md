# Ternak TT — Code Audit & Refactor (v1.0.29)

> This document explains the code review and refactor delivered on the
> `claude/code-audit-3bc1d1269de68019847d00a9911f2ed4` branch. Everything here is a
> correctness, safety, and hygiene pass — **no new spoofing capabilities or
> identifiers were added.**

## Background

Ternak TT is a Zygisk (Magisk / KernelSU / APatch) module that rewrites the
device-identity strings a whitelisted set of apps read at runtime. To understand
the fixes it helps to see the four cooperating layers, because a change in one
layer usually has a mirror in another.

**1. The native CLI — `jni/ternak-tt.cpp` → `bin/ternak-tt`.** A small root
binary. `freshen` rolls a new persona (`gen_identity()` picks a random entry
from a Pixel pool in `pool_tt.hpp`), writes `identity.prop`, then calls
`apply_native()` (~60 `resetprop-rs -n` calls) and `generate_mount_files()`
(synthetic `build.prop` per partition + `settings_secure.xml`). Other
subcommands: `seed` (post-fs-data), `apply-boot` (service), `status`,
`lock`/`unlock`, `rollback`, `targets`.

**2. The Zygisk shared object — `jni/main.cpp` + `jni/companion.cpp` →
`libternak_tt.so`.** For every app spawn, `preAppSpecialize` asks the companion
"is this package a target?" over the Zygisk UDS. If yes, the companion returns
the `identity.prop` blob and `postAppSpecialize` installs the Java hooks: it
overwrites `Build.*` static fields (L1) and registers a native
`SystemProperties.native_get(String,String)` hook (L2). The companion also forks
a child that `setns()`es into the target's mount namespace and bind-mounts the
synthetic `build.prop`/xml files so they are visible only to that app.

**3. The shell layer — `helpers.sh`, `rotate_ids.sh`, `action.sh`, `service.sh`,
`post-fs-data.sh`, `customize.sh`.** `action.sh` is the one-tap entry point:
`freshen` then `rotate_ids.sh all` (SSAID wipe, GAID, wlan MAC, BT MAC, device
name). `helpers.sh` is the shared library (logging, SELinux ref-counting,
`identity_get`/`identity_persist`, UUID/MAC generators).

**4. Build & release — `build.sh`, `.github/workflows/build.yml`.** `module.prop`
is the single source of truth for the version; the workflow auto-bumps the
patch, builds both variants across four ABIs, and generates `update.json` +
`release_notes.md`.

> [!NOTE]
> Layer consistency is the recurring theme. The hook layer (`main.cpp`) and the
> shell layer (`rotate_ids.sh`) both read the *same* `identity.prop`, so if one
> drifts from the other the persona becomes internally inconsistent — exactly the
> class of bug the v1.0.19 `sync_device_name` rewrite fixed, and the class this
> audit keeps guarding.

## Intuition

The headline finding is a *silent no-op*: an entire ~120-line subsystem in
`main.cpp` — a `/proc/mounts` sanitizer plus `openat`/`__openat`/
`android_get_device_api_level` PLT hooks — exists but `install_proc_sanitizer()`
is **never called** from anywhere in the module lifecycle. Grepping the tree
returns exactly one hit: its own definition. So it compiled into every shipped
`.so`, cost binary size and reader confusion, and did nothing.

The second finding is a classic latent hazard. The companion forks a child to
enter the target's mount namespace. After `fork()` in a process that already has
a background thread (the death-reaper `std::thread`), the child ran
`std::string` concatenation and `__android_log_print`. Between `fork()` and
`exec()` only *async-signal-safe* calls are legal, because the child inherits a
frozen snapshot of every mutex — if the reaper thread held the malloc or liblog
lock at the instant of `fork()`, the child deadlocks forever holding a namespace
it will never mount into.

A concrete way to picture it: imagine the reaper thread is halfway through
`malloc()` (holding the allocator lock) when a target app spawns and triggers
`fork()`. In the parent, the reaper finishes and releases the lock. In the
child, that lock is *already* marked held by a thread that does not exist in the
child — so the child's first `std::string` allocation blocks on a lock nobody
will ever release.

The remaining findings are smaller but real: a debug-only hook that leaked the
very property it claimed to "suppress"; a fallback UUID generator that produced
a malformed (non-RFC-4122) value; four different hardcoded version strings that
had all drifted from `module.prop`; and a CI extractor that made every release's
"What's new" section empty because it searched for the wrong heading depth.

## Code

**Removed dead code (`jni/main.cpp`, −170 lines).** Deleted
`install_proc_sanitizer()` and everything only it referenced: `hook_openat`,
`is_sensitive_proc_path`, `find_libc_dev_inode`, `hook_api_level`, the
`orig_openat`/`orig_api_level` trampolines, and the `memfd_create` helper (plus
the now-unused `<sys/mman.h>` / `<sys/syscall.h>` includes). Also removed the
unused `MOUNTDIR` constant and a duplicate `BIND_ENTRIES[]` — the authoritative
table lives in `companion.cpp`. This also retires the old "memfd file-size
fingerprint" issue #28, which is moot once the code is gone.

**Async-fork-safe mount child (`jni/companion.cpp`).** `do_mounts_via_fork()`
now allocates nothing on the heap in the child; it builds paths with stack
buffers (`snprintf`), performs only raw syscalls (`open`/`setns`/`access`/
`mount`), and reports results to the parent through a small POD struct over the
pipe:

```cpp
struct MountReport {
    uint32_t ok, fail, skip, skip_src, skip_dst;
    int32_t  stage;   // 0 = bind loop ran, 1 = open ns failed, 2 = setns failed
    int32_t  err;     // errno for stage 1/2
};
```

All `__android_log_print` calls moved to the parent, after `waitpid`. The bind
order, the `access(dst)` guard, `MS_BIND`, and the exact `child mount for pid=…`
/ `setns->target failed` log strings (which `summarize.sh` greps) are unchanged.

**SUPPRESS leak (`jni/main.cpp`, debug variant).** `native_get_long` /
`native_get_boolean` read the real property *before* labelling a key
`SUPPRESS`, leaking it. They now short-circuit like `native_get_int` already
did:

```cpp
} else if (tt_should_suppress_key(k)) {
    label = "SUPPRESS";   // keep def, do not read real prop
} else { /* read real prop */ }
```

**Version single-sourcing (`jni/ternak-tt.cpp`, `customize.sh`,
`webroot/index.html`).** A new `module_version()` reads `module.prop` at runtime,
so the CLI banner (was `v1.0.1`) and synthetic `build.prop` header (was
`v1.0.3`) always match the shipped version. `customize.sh` reads it from
`$MODPATH`; the WebUI header is now a placeholder that `app.js` fills from
`module.prop`.

**Conformant UUID fallback (`helpers.sh`).** The non-`/proc` path skipped hex
digit 13 and never set the RFC 4122 variant nibble. It now slices correctly and
picks a variant nibble from `{8,9,a,b}`.

**CI + docs.** The `release_notes.md` extractor in `build.yml` now matches
`## vX.Y.Z` by prefix (it was looking for `### vX.Y.Z` with an exact whole-line
match, so `(Unreleased)`/date suffixes never matched and "What's new" was always
empty). `CHANGELOG.md` lost a duplicate `# Changelog` H1 and the orphaned
`v1.0.19` section was restored to chronological order.

> [!WARNING]
> The `build.yml` change is **not** in the pushed branch — GitHub rejects
> workflow-file edits from tokens without `workflow` scope. The one-block patch
> is reproduced in the PR description for a maintainer to apply.

## Verification

No Android NDK is available in the review environment, so validation used a host
harness with stub bionic/JNI headers:

- **`clang++ -std=c++20 -fsyntax-only -Wall`** on `main.cpp` and `companion.cpp`
  with `zygisk.hpp` (fetched) + JDK `jni.h` + stub `android/log.h` and
  `sys/system_properties.h`. Both pass (remaining warnings are pre-existing
  release-build dead-code that `--gc-sections` strips).
- **Full host compile** of `ternak-tt.cpp`; ran `targets` and the usage banner.
  After staging a real `module.prop`, the banner correctly prints
  `Ternak TT v1.0.28`.
- **`sh -n` / `bash -n`** on all shell scripts; **`node --check`** on `app.js`;
  **PyYAML** parse of `build.yml`.
- **UUID fallback** exercised under both `dash` and `bash`: 5/5 outputs match
  `^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$`.
- **CI extractor** simulated against the new `CHANGELOG.md` for `v1.0.29`,
  `v1.0.28`, and `v1.0.19` — all three sections now extract correctly.

**Manual on-device QA for the maintainer:**

1. `./build.sh` with `ANDROID_NDK_HOME` set (both variants build).
2. Flash the debug zip, reboot, tap **Action**. Confirm `freshen` +
   `rotate_ids all` run and `identity.prop` populates.
3. `su -c 'ternak-tt'` shows the real version in the banner.
4. Launch a target app; in `logcat -s TernakTTCompanion:V` confirm a
   `child mount for pid=… N ok` line (mount path unchanged).
5. Re-launch several targets in a row to exercise the reaper + mount fork
   repeatedly; confirm no hangs.

## Alternatives

**Proc-sanitizer: remove vs. wire it up.**

| Remove (chosen) | Wire it into `postAppSpecialize` |
|---|---|
| Smaller `.so`, less reader confusion, no behavior change (it was already a no-op) | Would activate `/proc/mounts` hiding + API-level spoof |
| Keeps the audit scoped to correctness/hygiene | Adds a new anti-detection capability + the memfd size-fingerprint (issue #28) + `openat`/`__openat` shared-trampoline bug — needs real-device testing |

**Mount child: stack buffers + parent logging (chosen) vs. pre-fork everything /
`posix_spawn` helper.**

| Stack buffers + parent logging (chosen) | Open all fds pre-fork, or exec a tiny helper |
|---|---|
| Minimal diff, mount semantics identical, removes the malloc/liblog-after-fork hazard | Fully sidesteps fork-safety, but is a larger rewrite of the IPC/handoff and risks changing mount timing/behavior |

## Suggested people to discuss with

- **Ilham311** — repository owner and author of the prior `#1–29` deep-review
  pass; has the most context on every file touched here (Zygisk hook design,
  companion mount flow, the shell rotation layer).
- **Whoever reviewed commit `7a2d0aa`** ("preserve SUPPRESS and default for
  `hook_prop_get_int`") — this PR extends that exact reasoning to the `long` /
  `boolean` variants, so that context is directly relevant.

## Quiz

<details>
<summary>1. Why did removing <code>install_proc_sanitizer()</code> change runtime behavior essentially not at all?</summary>

- **A.** Because `--gc-sections` already stripped it — *close, but that only affects binary size, not whether it ran.*
- **B. Because it had zero call sites — it was never invoked in the module lifecycle. ✅** Grep shows only its definition; `postAppSpecialize` never called it.
- **C.** Because the hooks failed silently at runtime — *no; they were never registered at all.*

</details>

<details>
<summary>2. What makes the pre-fix mount child unsafe?</summary>

- **A. Calling <code>std::string</code>/<code>__android_log_print</code> (malloc + liblog locks) after <code>fork()</code> in a process that has a background thread. ✅** Only async-signal-safe calls are legal between `fork()` and `exec()`.
- **B.** Using `setns()` — *no, that syscall is fine.*
- **C.** The pipe read in the parent — *no, that is standard.*

</details>

<details>
<summary>3. Why was every release's "What's new" section empty?</summary>

- **A.** `CHANGELOG.md` had no version sections — *it did.*
- **B. The extractor searched for <code>### vX.Y.Z</code> with an exact line match, but the file uses <code>## vX.Y.Z</code> with <code>(Unreleased)</code>/date suffixes. ✅**
- **C.** `release_notes.md` was never uploaded — *it was; only the section was blank.*

</details>

<details>
<summary>4. Why is <code>native_get_int</code> untouched while <code>native_get_long</code>/<code>native_get_boolean</code> were fixed?</summary>

- **A. <code>native_get_int</code> already short-circuited suppressed keys; the other two read the real prop first, then only relabelled it. ✅**
- **B.** The int hook is release-only — *no; all three are debug-only.*
- **C.** Integers cannot leak — *the leak is about reading the real value, independent of type.*

</details>

<details>
<summary>5. Why read the version from <code>module.prop</code> at runtime instead of bumping the hardcoded strings?</summary>

- **A.** Runtime reads are faster — *irrelevant.*
- **B. <code>module.prop</code> is the single source of truth the release pipeline rewrites, so hardcoded copies inevitably drift; reading it at runtime removes the drift permanently. ✅**
- **C.** You cannot bump strings in C++ — *you can; it just does not stay in sync.*

</details>
