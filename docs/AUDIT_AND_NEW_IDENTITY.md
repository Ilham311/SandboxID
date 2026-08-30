# SandboxID — Engineering Audit & "New Identity" Program

> **Audience:** SandboxID engineering / maintainers.
> **Type:** Code audit + rebrand/architecture proposal. Single shareable document.
> **Repo:** `Ilham311/SandboxID` · **Branch reviewed:** `feat/l3-identity-hardening`
> **Tip commits:** `e0eb635` (native SIM auto-rotate + SubscriptionInfo/per-subId), `bfd8e1c` (.gnu_debugdata + AdServices).
> **Method:** static read of the C++/JNI core, shell lifecycle, WebUI, CI, and build system; local mirror of the CI checks (`tools/validate.sh`) inspected. No device boot test was run — device/CI validation is still pending (see F‑1).
> **Rationale policy:** conclusions and concise rationale only; no internal chain-of-thought.

---

## 0. Executive summary

SandboxID is a genuinely well-engineered Zygisk/Magisk module. The identity model is coherent (one persona seed → property layer L2/L7, native-read layer L9, and Binder layer L3 all agree), the companion process shows careful async-signal-safe `fork()` discipline, and the synthesis math (Luhn/IMEI/IMSI/ICCID/MEID) is correct. The code reads like it was written by someone who understands the platform.

The problems are not in the hot path — they are in **verification, robustness, and one correctness invariant that undercuts the module's own purpose**:

1. **Verification gap (High).** The unit tests are deleted in the working tree, CI never runs `tools/validate.sh` or the tests, and the branch carrying the largest recent change (the L3 LSPlant work) is not in the CI trigger list. The riskiest code — inline hooks that can brick boot — ships least verified.
2. **Detectability regression (High).** Every persona is forced to carry a valid Indonesian operator and `SIM_STATE_READY`. The "no SIM" state is unreachable by default, so *"always exactly one valid ID carrier, never absent"* becomes a stable invariant a detector can key on — the opposite of the module's goal.
3. **Robustness (Medium ×3).** The self-ELF resolver reads `/proc/self/maps` into a fixed 512-byte buffer (truncation → L3 silently disabled) and indexes the primary ELF's symbol tables without the bounds checks its own MiniDebugInfo path already has.
4. **Defense-in-depth (Medium).** `CMD_GET_IDENTITY` on the companion socket has no peer authentication, while the mount/hide commands do.

None of these is a remotely-exploitable "Critical" in normal use, and I am not inflating one to fill the row. The recommended sequence is: **restore the test/CI safety net first**, then fix the SIM invariant and the ELF robustness issues, then run the rebrand as a separate, additive program.

The **"New Identity" rebrand** is mechanically straightforward but has one hard constraint: a Magisk module's `id` *is* its install directory (`/data/adb/modules/sandboxid`), hardcoded in 41 places. You cannot rename in place — you ship a new `id` and migrate user state on first install. The plan below makes the id a single build-time parameter, provides an idempotent migration in `customize.sh`, and stages the roll-out canary → beta → full. Crucially, the intentionally-disguised callback classes (`androidx.core.os.EnvCompatState` / `HandlerCompatRef`) must **not** be rebranded — they are camouflage, and branding them would be a step backward.

---

# Part A — Professional audit report

## A.0 Findings at a glance

| ID | Severity | Area | Finding | Fix difficulty | Fix risk |
|----|----------|------|---------|----------------|----------|
| F‑1 | **High** | CI / QA | Tests deleted in tree; CI never runs `validate.sh`/tests; L3 branch not built by CI | Low | Low |
| F‑2 | **High** | Correctness / anti-fingerprint | SIM presence + valid operator forced on every persona; "no SIM" unreachable | Medium | Medium |
| F‑3 | Medium | Robustness | `lsparself` reads `/proc/self/maps` into fixed `char[512]` → truncation disables L3 | Low | Low |
| F‑4 | Medium | Robustness / safety | Primary-ELF `fill()` lacks the bounds checks the MiniDebugInfo path has → OOB read on malformed libart | Low | Low |
| F‑5 | Medium | Defense-in-depth | `CMD_GET_IDENTITY` has no peer auth while mount/hide do | Low | Low |
| F‑6 | Low | Coherence | `GSM_SIM_STATE` default `""` (config) vs L3 `V_SIM_STATE` hardcoded `"5"` | Low | Low |
| F‑7 | Low | Hygiene | Stale "TEMPORARY / remove before merging" CI trigger comment | Trivial | None |
| G‑* | Positive | — | Async-signal-safe companion, correct synthesis math, AppLog matcher ordering | — | — |

Severity model: **High** = defeats a core guarantee (verification or undetectability); **Medium** = robustness/hardening that only bites on edge inputs; **Low** = coherence/hygiene. No true Critical (nothing attacker-reachable with data-loss/RCE in normal operation) — stated plainly rather than manufactured.

---

## A.1 High

### F‑1 — The riskiest code ships least verified
**Where:** `.github/workflows/build.yml:3-13` (triggers), `tools/validate.sh` (full local mirror), `jni/CMakeLists.txt:33-43` (`SBX_BUILD_TESTS`), working-tree deletions of `tests/*`.

**Evidence:**
- CI triggers on `main`, `master`, `fix/dobby-arm64-build`, and `v*` tags only. The current work lives on `feat/l3-identity-hardening`, which **is not built by CI**. There is **no `pull_request:` trigger**, so PR #55 gets no CI signal.
- Grep of the workflow for `validate.sh`, `SBX_BUILD_TESTS`, `ctest`, `pull_request`, `tests/` → **no matches.** CI lints shell and runs `build.sh`; it never runs the unit tests or the L3 stub syntax-check.
- `validate.sh` references `tests/carrier_test.cpp`, `tests/native_read_test.cpp`, `tests/ident_synth_test.cpp`, `tests/validate_data.py`, and `tests/l3stub/l3_syntax_check.cpp`; `git status` shows all of these **deleted** in the working tree. `CMakeLists.txt:39-40` still wires `carrier_test`/`native_read_test` into `add_test`.

**Impact:** LSPlant + Dobby inline hooks run in every targeted app's zygote child; a bad symbol resolve or InitInfo drift can hang or crash boot. That is exactly the class of regression the deleted `l3stub` syntax-check and the tests were built to catch, and nothing runs them. This corroborates the standing "device/CI test pending" note.

**Recommended fix (Low difficulty, Low risk):** restore the deleted test files (from `e0eb635`/history), add a `validate` CI job that runs `tools/validate.sh` (installs `clang++`, `shellcheck`, `python3`), add a `pull_request:` trigger, and add `feat/**` to push triggers. Gate release steps on `main`/tags as today. See diff D‑1.

### F‑2 — Every persona always has a valid SIM (undetectability regression)
**Where:** `jni/main.cpp:92-103` (`rotate_sim_operator`), `jni/main.cpp:1217` (call), `jni/sbx_lsplant.hpp:487` (`have_sim`), `jni/sbx_lsplant.hpp:495-496` (passthrough), `jni/config.hpp:92-100` (`ID_CARRIERS`).

**Evidence:**
- `rotate_sim_operator()` picks one of the six `ID_CARRIERS` from the seed and **unconditionally** sets `g_identity["GSM_OPERATOR_NUMERIC"] = c.numeric` (`main.cpp:103`) whenever no manual `carrier.conf` pinned one. So by default `GSM_OPERATOR_NUMERIC` is *always* a real Indonesian MCC/MNC.
- In L3, `const bool have_sim = !v.op_num.empty();` (`sbx_lsplant.hpp:487`). Because op_num is always non-empty, `have_sim` is always true, and the guarded passthrough `if (!have_sim && (vid == V_SIM_STATE || …))` at `:495-496` is **effectively unreachable**.

**Impact:** The module can never present a plausible "no SIM / airplane / Wi‑Fi‑only tablet" device. A fingerprinting SDK that sees *every* SandboxID device carrying exactly one valid ID‑carrier with `SIM_STATE_READY` — and never an empty/absent SIM — has a stable population invariant to detect. This directly undercuts the module's stated purpose.

**Recommended fix (Medium/Medium):** gate SIM presence on the persona seed (e.g. ~15–20% of personas get empty `op_num`, restoring reachability of the `:495` passthrough), and make `ID_CARRIERS` selection carry through the empty case coherently across L2/L9/L3. See diff D‑4. Risk is Medium because it changes observable behavior and needs a device coherence pass (property vs Binder vs file all agreeing on "no SIM").

---

## A.2 Medium

### F‑3 — `/proc/self/maps` parsed through a fixed 512-byte buffer
**Where:** `jni/lsparself.hpp:155-188` (`findInMaps`), specifically `char line[512]` + `fgets` at `:157`, `:161`.

**Evidence:** `fgets(line, sizeof(line), fp)` truncates any maps entry longer than 511 bytes. The pathname is last on the line; deep app-lib paths (scoped storage, long package names, nested APK asset paths) can exceed that. On truncation the suffix match against the library name fails or matches the wrong mapping.

**Impact:** If the `libart.so` mapping line is truncated, symbol resolution returns 0, `lsplant::Init()` fails, and **all L3 hooks silently disable** — the failure mode called out in `lsparself.hpp`'s own header comment. Silent, environment-dependent, and hard to reproduce.

**Fix (Low/Low):** replace the fixed buffer with `getline()` (heap-grown, unbounded). See diff D‑2. This mirrors the `getline`-based maps parsing already used elsewhere (`main.cpp:463`, `:787`, "Bug #4").

### F‑4 — Primary-ELF symbol table indexed without bounds checks
**Where:** `jni/lsparself.hpp:233-241` (`fill` lambda) vs `:295-296` (`indexInnerElf`, which *does* check).

**Evidence:** `indexInnerElf()` validates `sym_sh->sh_offset + sym_sh->sh_size > n` and the same for the string table before dereferencing. The primary-image `fill()` lambda performs no equivalent check on `sh_offset`/`sh_size`/`sh_link` against the mmap size; it trusts `symsh->sh_link` (guarded) but then reads `sh_offset`-based pointers directly.

**Impact:** On a malformed or truncated `libart.so`, `out.syms`/`out.str` can point past the mapping → out-of-bounds read (crash) during scanning. Real-world exploitability is low (the ELF is a trusted on-device system file, not attacker-controlled), so this is robustness, not a live vuln — but a corrupt/partial system image would crash the hosting app process.

**Fix (Low/Low):** add the same `sh_offset + sh_size <= image_size_` and `strsh` bounds checks to `fill()`. See diff D‑3.

### F‑5 — `CMD_GET_IDENTITY` has no peer authentication
**Where:** `jni/companion.cpp:495-507` (peer creds read, then `CMD_GET_IDENTITY` served) vs `:593` (`CMD_DO_MOUNTS` auth) and `:615` (`CMD_DO_HIDE` auth).

**Evidence:** The companion reads `SO_PEERCRED` into `peer` and logs it, then serves `CMD_GET_IDENTITY` **without** checking `have_peer`/`peer.uid`. The mount and hide commands both require `have_peer && peer.uid == 0 && (pid_t)pid == peer.pid`.

**Impact:** The identity blob (synthetic IMEI/IMSI/ICCID/persona) is readable by any process that can reach the companion socket. In the normal Zygisk model that socket is only reachable from module code in zygote children, so the practical exposure is limited — but the asymmetry with the mount/hide guards is a defense-in-depth gap, and identity blobs are the module's most sensitive runtime data.

**Fix (Low/Low):** apply the same `have_peer` gate (at minimum) to `CMD_GET_IDENTITY`. Low risk because legitimate callers already satisfy it.

---

## A.3 Low

### F‑6 — SIM-state default disagrees between layers
**Where:** `jni/config.hpp:63` (`{"GSM_SIM_STATE", ""}`) vs `jni/sbx_lsplant.hpp:431` (`case V_SIM_STATE: return "5";`).

**Evidence:** L2's `GSM_SIM_STATE` default is empty; L3's `getSimState()` returns `"5"` (`SIM_STATE_READY`). Today F‑2 masks this (op_num is always set, so the higher-level SIM path is always "present"), but once F‑2 is fixed the two layers must be reconciled or a detector can compare the property against the Binder getter. Fold the reconciliation into the F‑2 fix.

### F‑7 — Stale CI comment
**Where:** `.github/workflows/build.yml:8-11`. The `fix/dobby-arm64-build` trigger is annotated "TEMPORARY … Remove this line before merging to main." The branch was merged (`766edd0`); the comment and possibly the trigger are now rot. Remove or convert to a documented `feat/**` pattern as part of D‑1.

---

## A.4 Positive findings (keep doing this)

- **Async-signal-safe companion (`companion.cpp`, "Bug #1b/#1c").** Correct discipline around `fork()` in a multithreaded process — no non-reentrant calls between fork and exec. This is the kind of bug most modules get wrong.
- **Synthesis math is correct (`sbx_ident_synth.hpp`).** IMSI is 15 digits, MEID's leading nibble is forced to A–F (avoids the decimal ESN/IMEI collision that trips validators), ICCID is 19 digits with a valid Luhn, and unresolvable carriers (Tri/Smartfren) leave `carrier_id` empty rather than fabricating a mismatched cid — itself a good anti-detection instinct.
- **AppLog matcher ordering (`sbx_native_read.hpp`).** `openudid`/`clientudid`/`cdid` are matched before `did`, avoiding substring shadowing.
- **Supply-chain hygiene in `build.sh`.** `zygisk.hpp` and `resetprop-rs` are pinned by commit + SHA‑256 and the build refuses on mismatch. Dependencies are pinned refs (`LSPLANT_REF=v2.0`, Dobby by commit). Extend this to `XZ_REF` (currently `master` — see F‑10 in Part F).

---

# Part B — The "New Identity" program (rebrand + architecture)

## B.1 What "identity" means for this module (the rename surface)

There are three distinct identity surfaces, with very different backward-compat weight:

| Surface | Examples | Compat weight | Rename? |
|---------|----------|---------------|---------|
| **Framework id** = install dir | `id=sandboxid` → `/data/adb/modules/sandboxid` (41 literal occurrences) | **Hard** — this *is* the module on disk | Yes, with migration |
| **User-facing brand** | `module.prop` name, README, WebUI title/logo/theme, CLI banner | Soft — cosmetic + docs | Yes |
| **Internal names** | C++ namespaces (`sandboxid`/`sbxnr`/`sbxlsp`/`sbxid`), `LOG_TAG`, file prefixes | None — invisible to users | Optional (SHOULD) |
| **Camouflage classes** | `androidx.core.os.EnvCompatState`, `HandlerCompatRef` | **Must stay disguised** | **No — do not rebrand** |

The camouflage classes deserve emphasis: they are deliberately named to blend into AndroidX so detectors scanning loaded classes see nothing branded. Renaming them to a new brand would *create* a fingerprint. Leave them exactly as they are; the rebrand explicitly excludes them.

### Naming (proposal, not a decree)
The current positioning is "privacy research & education." Keep a name that stays consistent with that framing (neutral, non-sensational, no words that read as evasion). Candidates, pros/cons:

- **`identikit`** — evokes composite/synthesis; neutral; short. Con: a real (unrelated) commercial product uses the word.
- **`persona`** — matches the code's own vocabulary (persona seed, personas.tsv). Pro: zero new concepts. Con: generic, common namespace collision.
- **`profilr` / `deviceprofile`** — descriptive, research-flavored. Con: less distinctive.

Recommendation: pick one you can hold as a trademark-clear GitHub org/repo; the plan below is parameterized on `PROJECT_ID` (new module id, lower-case) and `PROJECT_NAME` (display), so the choice can be deferred to the last commit.

## B.2 Constraints & principles

1. **A module id cannot be renamed in place.** Magisk/KSU/APatch key everything off the install directory. The migration is *new id + copy user state + retire old id*, done idempotently in `customize.sh`.
2. **Preserve every user artifact.** `target.txt`, `persona.override`, `carrier.conf`, `identity.prop`(+`.bak`), `identity.mode` must survive the move byte-for-byte.
3. **Keep the `sandboxid` CLI name working through a deprecation window.** The `system/bin/sandboxid` PATH shim (`build.sh:154-168`) is a public entrypoint; ship both `sandboxid` and the new name as shims, both exec'ing the same per-ABI binary, and warn on the old one.
4. **No behavior change rides along with the rename.** The rebrand PR must be provably behavior-neutral (byte-identical synthesis for a fixed seed). Bug fixes (Part A) land separately, before.
5. **Do not touch camouflage.** (B.1.)

## B.3 Architecture changes (make the id a single knob)

Today `id` is a compile-time constant baked into `config.hpp` paths and echoed literally across scripts. Target state:

- **One source of truth.** Introduce `SBX_MODULE_ID` (a compile definition) and derive every path in `config.hpp` from it (`"/data/adb/modules/" SBX_MODULE_ID`). `build.sh` reads `id=` from `module.prop` and passes `-DSBX_MODULE_ID=<id>` to CMake. This turns 41 literals into one. See diff D‑5.
- **Scripts read `MODDIR` from the framework**, never hardcode the path. Most already receive `MODDIR`; audit `scripts/**` and replace any literal `/data/adb/modules/sandboxid` with `${MODDIR:?}` or a single `helpers.sh` constant.
- **WebUI brand tokens** move to CSS custom properties + a single `BRAND` object in `app.js`, and the `localStorage` theme key migrates from `sbx-theme` (read old, write new).

This refactor is worth doing **independently of any rename** — it removes a whole class of "missed a path" bugs and makes the eventual rename a one-line change plus the migration shim.

## B.4 Data migration plan (old install → new id)

**Script:** additive block at the top of the new module's `customize.sh`.

1. **Detect** `/data/adb/modules/sandboxid` (old id) present and `/data/adb/modules/<PROJECT_ID>` being installed.
2. **Copy user state** (only if the destination file is absent, so re-install never clobbers newer config): `target.txt`, `persona.override`, `carrier.conf`, `identity.prop`, `identity.prop.bak`, `identity.mode`.
3. **Verify** each copy with a size + SHA‑256 compare; abort migration (leave old intact) on mismatch and print a manual-migration hint.
4. **Retire old** only after verification: write `disable` into the old module dir (Magisk convention) rather than deleting, so rollback is `rm .../sandboxid/disable`.
5. **Log** a migration record to the new module's log dir.

**Verification harness:** a `tests/migration_test.sh` that stages a fake old-module tree with known files, runs the migration block, and asserts checksums match and the old dir is disabled-not-deleted.

**Rollback:** (a) uninstall new module; (b) `rm /data/adb/modules/sandboxid/disable`; (c) reboot. Because step 4 never deletes, rollback is always available for one release cycle. Document a hard cutover (delete old) only from the *next* major version.

## B.5 CI/CD & staged rollout

- **Canary (internal):** debug variant, `feat/**` branch build in CI (F‑1), install on maintainers' own devices, verify boot on each target ABI/Android version (the `CMakeLists.txt:47` warning is not a formality — inline hooks brick boot when wrong).
- **Beta (opt-in):** tag `vX.0.0-beta.N`, publish as a GitHub *pre-release*; `update.json` for beta lives on a separate channel URL so stable users are not offered it.
- **Full:** promote to stable tag, flip `updateJson` to the new repo's `releases/latest`. Keep the old repo's `update.json` pointing at a **final "please migrate" release** for one cycle so existing installs get a notice, not silence.

---

# Part C — Tactical implementation plan

## C.1 Prioritized backlog (MUST / SHOULD / CAN)

| Prio | Task | Effort | Depends on |
|------|------|--------|------------|
| **MUST** | T1 Restore tests + wire `validate.sh` + `pull_request` into CI + build `feat/**` (F‑1, F‑7) | 0.5 d | — |
| **MUST** | T2 `getline` in `findInMaps` (F‑3) | 0.5 d | T1 (to get CI signal) |
| **MUST** | T3 Bounds-check primary-ELF `fill()` (F‑4) | 0.5 d | T1 |
| **MUST** | T4 Peer-auth `CMD_GET_IDENTITY` (F‑5) | 0.25 d | T1 |
| **SHOULD** | T5 Seed-gated SIM presence + L2/L3 SIM-state reconcile (F‑2, F‑6) | 2–3 d | T1; device coherence pass |
| **SHOULD** | T6 Parameterize module id via `SBX_MODULE_ID` (B.3) | 1 d | T1 |
| **SHOULD** | T7 Pin `XZ_REF` to a commit; add Dependabot/renovate (F‑10) | 0.5 d | — |
| **CAN** | T8 Rebrand: name, WebUI tokens, README/CHANGELOG, migration shim, staged release (Part B) | 3–5 d | T6 |
| **CAN** | T9 Rename internal namespaces/`LOG_TAG` (cosmetic) | 0.5 d | T6, T8 |

Sequencing rationale: **safety net (T1) first**, then the three low-risk robustness/hardening fixes (T2–T4) which T1 now protects, then the behavior-changing SIM fix (T5) with a device pass, then the id-parameterization (T6) that makes the rebrand (T8) a small diff. Cosmetic renames (T9) last.

## C.2 Step-by-step for the top 5

**T1 — CI safety net.** (1) `git checkout e0eb635 -- tests/` to restore the deleted tests + `l3stub`. (2) Add diff D‑1: `pull_request:` trigger, `feat/**` push trigger, a `validate` job that apt-installs `clang`, `shellcheck`, `python3` and runs `tools/validate.sh`; keep release steps gated to `main`/tags. (3) Remove the stale `fix/dobby-arm64-build` trigger + comment. (4) Open PR; confirm the `validate` job is green and actually executed the tests (check the log for `RESULT: PASS`).

**T2 — `findInMaps` getline.** (1) Apply D‑2. (2) Add a host test that feeds a synthetic maps line with a 700-char path and asserts the suffix match still succeeds. (3) `validate.sh` green.

**T3 — `fill()` bounds.** (1) Apply D‑3. (2) Extend the L3 stub syntax-check or add a small unit that constructs a truncated section header and asserts `fill()` leaves the `SymTab` empty rather than pointing OOB. (3) Green.

**T4 — `CMD_GET_IDENTITY` auth.** (1) Add `if (!have_peer) { LOGW(...); continue; }` (or reject) before serving the blob, matching the mount/hide pattern. (2) Manually confirm a normal target app still receives its identity (auth is satisfied by the legitimate path). (3) Green.

**T5 — Seed-gated SIM.** (1) In `rotate_sim_operator()`, derive a presence bit from the persona seed; when "absent", leave `GSM_OPERATOR_NUMERIC` unset and set `GSM_SIM_STATE` to the absent code. (2) Make L3 `have_sim` honor the empty case (it already does — restoring reachability of `sbx_lsplant.hpp:495`). (3) Reconcile `V_SIM_STATE` return with `GSM_SIM_STATE` (F‑6): return the configured value, not a hardcoded `"5"`. (4) **Device coherence pass**: verify property (`getprop gsm.operator.numeric`), Binder getter (`TelephonyManager.getSimState`), and file readers all agree for both present and absent personas. (5) Update `data/*.tsv` docs if a presence column is added.

## C.3 Starter PRs (small & safe) + the one large PR

- **PR‑A (starter):** "ci: run validate.sh on PRs and feature branches; restore host tests" — T1. Small, mechanical, immediately raises the floor. Sample message:
  `ci(validate): run tools/validate.sh on pull_request + feat/** ; restore tests/*`
- **PR‑B (starter):** "fix(l3): getline maps parse + bounds-check primary ELF symtab" — T2+T3. Two tight, well-tested robustness fixes.
- **PR‑C (starter):** "fix(companion): require peer creds for CMD_GET_IDENTITY" — T4.
- **PR‑D:** "feat(sim): seed-gated SIM presence; reconcile L2/L3 sim-state" — T5. Behavior change → its own PR with device notes.
- **PR‑E (starter):** "refactor(config): derive module paths from SBX_MODULE_ID" — T6. Behavior-neutral; unblocks rebrand.
- **PR‑F (large):** "feat: New Identity rebrand (`PROJECT_NAME`) + migration" — T8/T9. One large PR *because* the rename touches ~30 files atomically and a half-renamed tree does not build/boot. Sample PR description skeleton:
  > **What:** Rebrand `sandboxid` → `PROJECT_ID`/`PROJECT_NAME`. New module id, WebUI theme + brand tokens, README/CHANGELOG/CREDITS, `customize.sh` migration from the old install, dual CLI shim (`sandboxid` deprecated alias).
  > **Not included:** camouflage classes (`EnvCompatState`/`HandlerCompatRef`) unchanged by design; no synthesis/behavior change (verified byte-identical for a fixed seed).
  > **Migration:** first install copies user state from `/data/adb/modules/sandboxid`, verifies checksums, disables (not deletes) the old module. Rollback documented.
  > **Testing:** `validate.sh` green; `tests/migration_test.sh` green; boot verified on arm64 Android 13/14 + one armeabi device.

---

# Part D — Sample diffs

> Illustrative unified diffs. Line offsets are from the reviewed tip; re-check with `git apply --check` before landing. Each is scoped to one finding.

## D‑1 — CI: run validate.sh on PRs & feature branches (F‑1, F‑7)
**Rationale:** the safety net that gates every other change. **Risk:** Low (adds coverage; release path unchanged). **Tests:** the job *is* the test — confirm `RESULT: PASS` in its log. **Migration:** none.

```diff
--- a/.github/workflows/build.yml
+++ b/.github/workflows/build.yml
@@
 on:
   push:
     branches:
       - main
       - master
-      # TEMPORARY: build this fix branch in CI to verify the Dobby/LSPlant build.
-      # Remove this line before merging to main (release steps below are gated to
-      # main/master, so a branch build only lints + compiles + uploads artifacts).
-      - fix/dobby-arm64-build
+      - 'feat/**'
     tags:
       - 'v*'
+  pull_request:
   workflow_dispatch:
@@
 jobs:
+  validate:
+    runs-on: ubuntu-latest
+    steps:
+      - uses: actions/checkout@v4
+      - name: Install toolchain
+        run: sudo apt-get update && sudo apt-get install -y clang shellcheck python3
+      - name: Run local CI mirror
+        run: bash tools/validate.sh
   build:
     runs-on: ubuntu-latest
+    needs: validate
```

## D‑2 — `findInMaps`: unbounded getline (F‑3)
**Rationale:** a truncated maps line silently disables all of L3. **Risk:** Low. **Tests:** synthetic 700‑char path line still matches. **Migration:** none.

```diff
--- a/jni/lsparself.hpp
+++ b/jni/lsparself.hpp
@@ bool findInMaps(std::string_view name, std::string& outPath, uintptr_t& outBase) {
         FILE* fp = fopen("/proc/self/maps", "re");
         if (!fp) return false;
-        char line[512];
+        char*  line = nullptr;
+        size_t cap  = 0;
+        ssize_t n;
         uintptr_t          best_base = 0;
         unsigned long long best_off  = ~0ULL;
         std::string        best_path;
-        while (fgets(line, sizeof(line), fp)) {
+        while ((n = getline(&line, &cap, fp)) != -1) {
             // start-end perms offset dev(maj:min) inode pathname
             unsigned long long start = 0, end = 0, off = 0;
             char perms[8] = {0};
             int  pos = 0;
             if (sscanf(line, "%llx-%llx %7s %llx %*x:%*x %*u %n",
                        &start, &end, perms, &off, &pos) < 4)
                 continue;
@@
         }
+        free(line);
         fclose(fp);
```

## D‑3 — Bounds-check the primary-ELF `fill()` (F‑4)
**Rationale:** mirror the checks `indexInnerElf()` already has, so a malformed `libart.so` yields an empty table instead of OOB pointers. **Risk:** Low. **Tests:** truncated section header → `SymTab` stays empty. **Migration:** none.

```diff
--- a/jni/lsparself.hpp
+++ b/jni/lsparself.hpp
@@ auto fill = [&](const ElfW(Shdr)* symsh, SymTab& out) {
             if (!symsh || symsh->sh_entsize == 0) return;
             if (static_cast<size_t>(symsh->sh_link) >= shnum) return;
             const ElfW(Shdr)& strsh = sh[symsh->sh_link];
+            // libart.so is trusted, but a corrupt/partial image must not point us
+            // past the mmap. Same guard as indexInnerElf().
+            if (symsh->sh_offset + symsh->sh_size > image_size_) return;
+            if (strsh.sh_offset  + strsh.sh_size  > image_size_) return;
             out.syms  = reinterpret_cast<const ElfW(Sym)*>(base + symsh->sh_offset);
             out.count = static_cast<size_t>(symsh->sh_size / symsh->sh_entsize);
             out.str   = reinterpret_cast<const char*>(base + strsh.sh_offset);
             out.strsz = static_cast<size_t>(strsh.sh_size);
         };
```

## D‑4 — Seed-gated SIM presence (F‑2, F‑6)
**Rationale:** restore reachability of the "no SIM" state so the population isn't a uniform tell. **Risk:** Medium — observable change; needs the device coherence pass in C.2/T5. **Tests:** distribution check over many seeds (~15–20% absent); L2/L3/file agree in both states. **Migration:** none (per-run).

```diff
--- a/jni/main.cpp
+++ b/jni/main.cpp
@@ static void rotate_sim_operator() {
     uint64_t seed = sbxnr::fnv1a(val("FINGERPRINT") + "|" + val("SERIAL") + "|" +
                                  val("ANDROID_ID"));
     uint64_t s = seed ^ 0x53494D53454CULL;              // "SIMSEL"
+    // Some real devices have no SIM (Wi-Fi tablets, airplane mode, eSIM-off).
+    // Gate presence on the seed so "no SIM" is reachable — a population where
+    // every persona always has a valid operator is itself a fingerprint.
+    if ((sbxnr::splitmix64(s) % 100) < 18) {            // ~18% no-SIM personas
+        g_identity.erase("GSM_OPERATOR_NUMERIC");
+        g_identity.erase("GSM_OPERATOR_ALPHA");
+        g_identity.erase("GSM_OPERATOR_ISO");
+        g_identity.erase("GSM_CARRIER_ID");
+        g_identity["GSM_SIM_STATE"] = "1";              // SIM_STATE_ABSENT
+        LOGI("SIM auto-rotate: no SIM (absent persona)");
+        return;
+    }
     const auto& c = sandboxid::ID_CARRIERS[sbxnr::splitmix64(s) % sandboxid::ID_CARRIERS_N];
     g_identity["GSM_OPERATOR_NUMERIC"] = c.numeric;
```
> Pair with `sbx_lsplant.hpp:431`: return the configured `GSM_SIM_STATE` from `V_SIM_STATE` instead of the hardcoded `"5"`, so the Binder getter matches the property (F‑6).

## D‑5 — One-knob module id (T6, unblocks rebrand)
**Rationale:** collapse 41 hardcoded paths to one build-time parameter. **Risk:** Low (behavior-neutral; default keeps `sandboxid`). **Tests:** build with default id → byte-identical paths; build with `-DSBX_MODULE_ID=persona` → all paths repoint. **Migration:** none by itself; enables B.4.

```diff
--- a/jni/config.hpp
+++ b/jni/config.hpp
@@
 namespace sandboxid {
 
-inline constexpr char MODDIR[]        = "/data/adb/modules/sandboxid";
-inline constexpr char IDENTITY_FILE[] = "/data/adb/modules/sandboxid/identity.prop";
-inline constexpr char MOUNTDIR[]      = "/data/adb/modules/sandboxid/mount";
-inline constexpr char TARGET_FILE[]   = "/data/adb/modules/sandboxid/target.txt";
+#ifndef SBX_MODULE_ID
+#define SBX_MODULE_ID "sandboxid"      // default; build.sh passes -DSBX_MODULE_ID=<module.prop id>
+#endif
+#define SBX_MODDIR "/data/adb/modules/" SBX_MODULE_ID
+inline constexpr char MODDIR[]        = SBX_MODDIR;
+inline constexpr char IDENTITY_FILE[] = SBX_MODDIR "/identity.prop";
+inline constexpr char MOUNTDIR[]      = SBX_MODDIR "/mount";
+inline constexpr char TARGET_FILE[]   = SBX_MODDIR "/target.txt";
```
> Apply the same `SBX_MODDIR "/..."` treatment to the remaining path constants (lines 15–26), and in `build.sh` add `-DSBX_MODULE_ID="$(grep '^id=' module.prop | cut -d= -f2)"` to the `cmake -S jni` invocation. Camouflage class names are **not** touched.

---

# Part E — QA, testing & monitoring

**Restore the pyramid (F‑1 is the keystone):**
- **Host unit tests** (fast, in CI): `carrier_test`, `native_read_test`, `ident_synth_test`, `validate_data.py` — restore and keep green. Add: a `findInMaps` long-path test (D‑2), a `fill()` truncation test (D‑3), a SIM-presence distribution test (D‑4), and `migration_test.sh` (B.4).
- **L3 stub syntax-check** (`tests/l3stub/l3_syntax_check.cpp`): the cheapest guard against InitInfo/field-order drift in the LSPlant contract. Restore it; it is the only host-side signal for the class of bug that silently disables all L3 hooks.
- **Device smoke matrix** (manual gate before any tag): boot each target ABI × Android version; verify with the WebUI "Uji" tab and a real detector app that L2/L9/L3 agree, for both a SIM-present and (post‑D‑4) a SIM‑absent persona.

**Monitoring:** the crash watchdog already scopes SIGABRT/FPE/ILL; add a boot-loop guard note to `service.sh` (if N consecutive boots log an L3 init failure, auto-fall back to `SBX_ENABLE_LSPLANT=OFF` behavior by skipping the L3 install) so a bad hook degrades to L2/L9 instead of a bootloop. Track "L3 init failed" as the top health signal.

---

# Part F — Security & dependency plan

| ID | Item | Current | Target |
|----|------|---------|--------|
| F‑8 | LSPlant | `LSPLANT_REF=v2.0` (pinned) | keep pinned; watch upstream for the next tag, bump deliberately |
| F‑9 | Dobby | LSPosed fork, pinned by commit | keep; good |
| **F‑10** | xz-embedded | `XZ_REF=master` (**floating**) | **pin to a commit/tag** — a moving `master` breaks reproducible builds and is a supply-chain risk |
| F‑11 | `zygisk.hpp`, `resetprop-rs` | commit + SHA‑256 pinned, verified in `build.sh` | model of good practice — keep |
| F‑12 | Automation | none | add **Dependabot** for GitHub Actions + a scheduled job that flags upstream LSPlant/Dobby/xz updates (they are git clones, not package deps, so a custom "check upstream ref" workflow, not the ecosystem updater) |

**Other security notes:** apply F‑5 (peer auth on `CMD_GET_IDENTITY`). Keep the `--exclude-libs,ALL` + strip in release (already present). The identity blob and `.bak` files under `/data/adb/modules/...` are root-only by directory permissions — keep it that way; never widen.

---

# Part G — DevEx & docs

- **CONTRIBUTING.md (missing — add it):** how to build (`ANDROID_NDK_HOME`, `build.sh`), how to run `tools/validate.sh` before pushing, the "verify boot on every ABI before shipping L3" rule, and the "don't rename camouflage classes" rule.
- **CHANGELOG.md (exists, 68 KB):** keep the Keep-a-Changelog style; add an `Unreleased` section and land each PR's entry with the PR.
- **README:** after the rebrand, add a top-of-file note that the project was formerly `SandboxID`, with a link to the migration guide, for one release cycle.
- **MIGRATION.md (new):** the B.4 flow in user terms — what moves, how to verify, how to roll back.
- **Usage snippets:** document the dual CLI (`PROJECT_ID …` and the deprecated `sandboxid …` alias) and the WebUI tabs. Note the deprecation date for the old alias.

---

# Part H — Communication & rollout

- **Release notes** per stage (canary/beta/full) stating what changed and, for the rebrand, leading with migration + rollback.
- **Deprecation schedule:**
  - vX.0 (rebrand): old id migrated automatically; old module *disabled, not deleted*; `sandboxid` CLI alias works with a warning.
  - vX.1 (+1 cycle): old repo `update.json` serves a final "migrate now" notice.
  - vX+1.0 (next major): hard cutover — migration block may delete the old id; CLI alias removed.
- **One-cycle overlap** everywhere: never remove a compatibility path in the same release that introduces its replacement.

---

# Appendix — references & method notes

- **Evidence base:** all findings cite `file:line` at the reviewed tip (`feat/l3-identity-hardening`, `e0eb635`). CI trigger claims verified against `.github/workflows/build.yml`; "CI never runs tests" verified by grep (`validate.sh|SBX_BUILD_TESTS|ctest|pull_request|tests/` → no matches).
- **Not verified (stated honestly):** no device boot was performed; F‑2/F‑5 device coherence and the L3 init path are asserted from code, not runtime. The device smoke matrix (Part E) is the gate that closes this gap.
- **External references:** Magisk module layout & `id`-as-directory (topjohnwu/Magisk module docs); LSPlant `InitInfo` contract (LSPosed/LSPlant); AOSP `carrier_list.textpb` (carrier_id values); GSMA TS.06 (IMEI/TAC), ITU‑T E.118 (ICCID), 3GPP TS 23.003 (IMSI/MCC/MNC) for the synthesis math already implemented in `sbx_ident_synth.hpp`.

*End of document.*
