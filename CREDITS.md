# Credits & References

SandboxID is [MIT](./LICENSE) licensed. It is built on **publicly documented
platform commands and techniques**. Where an approach was learned from another
open-source module, that project is credited below. **No third-party source
code is copied into this repository** — only documented, idiomatic one-line
commands (`pm clear`, `am force-stop`, `killall`, `resetprop -n`), the general
**technique** (which properties a locked/verified device sets, which mounts are
root-manager traces), and **factual identifier lists** (AOSP / QEMU-goldfish /
LineageOS property *names* — which are facts about a platform, not creative
expression) are adopted. Each such feature is **reimplemented from scratch** in
this repo's own code, so SandboxID stays MIT and the referenced projects retain
their own licenses. In particular, **GPL/AGPL-licensed projects are credited for
ideas only, never code** — copying their source would relicense this module, and
crediting does not cure that.

---

## Platform primitives (Android)

The app stop + data wipe and the identifier writes use the documented
`adb shell` / on-device command surface. `--user <id>` always goes **after**
the verb (verified against the on-device usage banners).

| Command | What it does | Source |
| --- | --- | --- |
| `pm clear --user 0 <pkg>` | "Delete all data associated with a package." | [Android — adb / app manager commands](https://developer.android.com/tools/adb#pm) |
| `am force-stop --user 0 <pkg>` | "Force-stop everything associated with `<package>`." | [Android — adb / activity manager commands](https://developer.android.com/tools/adb#am) |
| `settings put --user 0 <ns> <k> <v>` | Write a Settings provider value (`secure`/`global`/`system`). | [Android — adb settings](https://developer.android.com/tools/adb) |
| `killall <process>` | Best-effort kill of a running process by name (fallback sweep). | toybox `killall`; technique below |

## Identifier model (Android)

- **Advertising ID** — a user-resettable UUID that must not be linked to the
  previous ID after a reset:
  <https://developer.android.com/identity/ad-id>
- **Android ID (SSAID), MAC, IMEI/serial scoping** — MAC is not app-accessible
  on Android 6+ (`getHardwareAddress()` → `null`), and IMEI/serial are
  restricted on Android 10+:
  <https://developer.android.com/identity/user-data-ids>

## Magisk module runtime

- **`resetprop`** (set system properties without `property_service`; `-n`, `-p`,
  `--delete`, `--file`) and the **boot-stage contract** used by this module
  (`post-fs-data` = blocking, pre-Zygote → file/prop only; `service.sh` =
  late-start, non-blocking → wait for `sys.boot_completed`; `action.sh` = runs
  post-boot with the framework available):
  <https://topjohnwu.github.io/Magisk/guides.html> and
  <https://topjohnwu.github.io/Magisk/details.html>

---

## Techniques referenced from other modules

- **PlayIntegrityFork** — [osm0sis/PlayIntegrityFork](https://github.com/osm0sis/PlayIntegrityFork)
  (GPL-3.0). Its `killpi.sh` uses `killall` to stop the relevant processes
  after refreshing identity/props. SandboxID adopts the same *technique* (a
  `killall <pkg>` sweep as a best-effort fallback after `am force-stop`) with
  its own original implementation; no PlayIntegrityFork code is copied.
  - PlayIntegrityFork's own credit chain (property-spoofing lineage):
    osm0sis ← chiteroman (PlayIntegrityFix) ← kdrag0n (ProtonAOSP /
    Universal SafetyNet Fix) ← Displax.

- **autopif.sh (canary fingerprint fetcher)** — [dannycreations' `autopif.sh`
  gist](https://gist.github.com/dannycreations/659e0b780e8b89ea5140c2d837ac2ed5)
  (no license stated). The *technique* of scraping Google's public Pixel pages
  (versions → factory-image → flash-station API → security bulletin) to derive
  the newest **canary** build fingerprint is adapted in this repo's `autopif.sh`.
  The scraping steps necessarily mirror the source because they follow Google's
  page structure, but the SandboxID script is **rewritten** for on-device
  Android `sh` and to *upsert* the persona pool (`personas.tsv`) — with a SoC
  allow-list and an offline no-op guard — instead of writing a PlayIntegrityFix
  `pif.json`. No gist code is copied verbatim; because the gist states no
  license, only the documented technique is reused, not its source.

### Anti-detection hardening (verified-boot, emulator, root/mount hiding)

These features adopt the **technique** and, where noted, **factual property-name
lists** from the projects below. All are **reimplemented** in this repo's own
code (`jni/config.hpp`, `jni/sandboxid.cpp`, `jni/main.cpp`,
`jni/sbx_native_read.hpp`, `jni/sbx_mountinfo.hpp`, `jni/companion.cpp`); no
source is copied.

- **reveny/Android-VBMeta-Fixer** — [reveny/Android-VBMeta-Fixer](https://github.com/reveny/Android-VBMeta-Fixer)
  (MIT). The *recipe* for a coherent locked/verified boot state via `resetprop`:
  `ro.boot.verifiedbootstate=green`, `ro.boot.vbmeta.device_state=locked`,
  `ro.boot.flash.locked=1`, `ro.boot.veritymode=enforcing`,
  `ro.boot.vbmeta.{hash_alg=sha256,avb_version=1.0,invalidate_on_error=yes}`,
  plus `ro.secure=1` / `ro.debuggable=0`. SandboxID sets these across its three
  coordinated prop surfaces. **Limitation:** a *genuine* `ro.boot.vbmeta.digest`
  requires the device's verified-boot key (out of scope); SandboxID emits a
  deterministic per-identity placeholder (`hex_from_seed(fnv1a(fingerprint|serial))`)
  and documents that it is not key-attested.

- **yubunus/Hide-My-Goldfish** — [yubunus/Hide-My-Goldfish](https://github.com/yubunus/Hide-My-Goldfish)
  (MIT). Technique + the *reference list of QEMU/goldfish/ranchu property names*
  an emulator exposes and a physical device does not. SandboxID's
  `is_emulator_prop()` matcher marks that family **absent** to target apps
  (original matcher, standard identifier names).

- **Magisk-Modules-Alt-Repo/ezme-nodebug** — [ezme-nodebug](https://github.com/Magisk-Modules-Alt-Repo/ezme-nodebug)
  (MIT). Reference list of **LineageOS / custom-ROM property names**
  (`ro.lineage.*`, `lineage.*`, `ro.modversion`, …); SandboxID's matcher marks
  these absent per-app. Property *names* only — no code.

- **snake-4/Zygisk-Assistant** — [snake-4/Zygisk-Assistant](https://github.com/snake-4/Zygisk-Assistant)
  (MIT). Technique for the **opt-in, default-off** root/mount-trace hider (F6):
  a forked companion `setns()` into the target's mount namespace, `MS_SLAVE|MS_REC`
  to isolate propagation, then reverse-order `umount2(…, MNT_DETACH)` of
  root-manager overlay/tmpfs mounts. SandboxID **deliberately diverges** (and says
  so in `jni/sbx_mountinfo.hpp`): it does **not** port the `unshare`-strip /
  `setresuid` PLT hooks (they fight this module's containment model), it **defers**
  the `libnativebridge had_error` fix (would pull in ELFIO / Apache-2.0), and it
  uses a deliberately **narrow** target selector that never touches this module's
  own persona binds, `MODDIR`, `/data`, or bare partition roots — because an
  over-aggressive unmount is itself a detectable signal. The selector is original,
  host-unit-tested code.

- **sensitive_props** — the *sensitive props* concept popularised by several
  **GPL-3.0** root-hiding modules (resetprop-based clearing/zeroing of props a
  retail device would not advertise). Used for the **idea only** — which
  adb/OEM-unlock tells to normalise, e.g. `sys.oem_unlock_allowed=0`. No code is
  read or copied; the clean-room boundary above applies.

- **reveny/Android-Native-Root-Detector** — [reveny/Android-Native-Root-Detector](https://github.com/reveny/Android-Native-Root-Detector).
  The repo is MIT-shelled but the actual detector is a **closed prebuilt
  `libreveny.so`**. SandboxID uses it **only as a black-box test oracle and a
  requirements checklist** (which signals to neutralize) — never disassembled,
  never copied. The on-device detection self-check (`selftest.sh`) is original
  and reports read-only signals; it does not embed or link anything from it.

---

## Reference modules studied for the device-identity roadmap

The projects below were **analysed** (read as prior art) while planning
SandboxID's device-identity coverage. Each entry states the project's license,
**what SandboxID actually takes** from it, and its honest **status** — because
crediting a project does not mean its feature already ships here. The clean-room
boundary at the top of this file governs every entry: **GPL/AGPL projects are
studied for ideas only, never code; a project with no license is used for
verifiable platform *facts* only (identifier names), never its expression; MIT
projects are reimplemented, and any vendored snippet would carry its notice
inline at the copy site.** URLs are given as `owner/module` locators — confirm
the exact slug before redistribution.

- **Device Faker** — `Seyud/DeviceFaker` (**GPL-3.0**). Studied for its
  *approach* to broadening spoof coverage beyond `Build.*` into
  runtime-queried identity (media DRM / Widevine id, sensor enumeration).
  **Idea only, no code** — porting any GPL source would relicense this module.
  *Status: studied; corresponding native layers are roadmap items, not yet
  implemented here.*

- **TargetedFix** — `VisionR1/TargetedFix` (**GPL-3.0**). Studied for the
  *per-target-package* application model (apply an identity only to selected
  apps). SandboxID already scopes effects to `target.txt`; the finer
  *per-package distinct identity* is a roadmap item. **Idea only, no code.**
  *Status: concept studied; per-package-distinct identities not yet
  implemented.*

- **MagiskHidePropsConf (MHPC)** — `Didgeridoohan/MagiskHidePropsConf`
  ([github.com/Magisk-Modules-Repo/MagiskHidePropsConf](https://github.com/Magisk-Modules-Repo/MagiskHidePropsConf),
  **MIT** per its README). Referenced for the *curated fingerprint-list*
  technique (`printslist` — pick a known-good certified fingerprint from a
  shipped list). SandboxID's analogue is its own `personas.tsv` / `devices.tsv`
  data pools and CLI picker; the *idea* of shipping a chooseable fingerprint
  list is the borrowed part, **reimplemented** in this repo's own shell + data.
  *Status: technique referenced; list-driven persona picking is implemented via
  `personas.tsv`/`devices.tsv` (the interactive per-device picker is a roadmap
  item).*

- **Device ID Changer** — `sidex15/Device-ID-Changer` (**AGPL-3.0**). Studied
  for the *user-facing identifier-reset* surface (Android ID / advertising ID /
  GSF-style ids). AGPL is the strongest copyleft here, so the caution is
  strictest: **idea only, never code, never a derived work.** SandboxID's SSAID
  / GAID rotation is independently implemented in `rotate_ids.sh` +
  `jni/sandboxid.cpp` against the documented `settings`/Ad-ID surface.
  *Status: independently implemented; no AGPL code or structure adopted.*

- **DeviceSpoofLab-Magisk** — `yubunus/DeviceSpoofLab-Magisk` (**MIT**; same
  author as the already-credited Hide-My-Goldfish above). Studied for its
  layout of a spoof "lab" across build-prop surfaces. **Technique only** — no
  source copied; SandboxID's prop surfaces predate and differ from it.
  *Status: technique studied; nothing new vendored.*

- **SpoofingCollection** — `mrx7014/SpoofingCollection` (**no license stated →
  all rights reserved**). Because no license is granted, **only verifiable
  facts are used — Samsung/Knox-family property *names* that are facts about the
  platform, not creative expression.** No file, script, structure, or wording
  from it is copied or adapted. *Status: property-name facts referenced only;
  Samsung/Knox persona profiles are a roadmap item and not yet implemented.*

- **FingerprintJS Android** —
  [github.com/fingerprintjs/fingerprintjs-android](https://github.com/fingerprintjs/fingerprintjs-android)
  (**MIT**). Used as a **detector oracle and requirements checklist** — which
  device signals a fingerprinting SDK actually reads (screen metrics, GPU/GLES
  renderer, sensor list, `statfs` totals, CPU/ABI) — to decide what SandboxID
  must keep coherent. Alongside the existing `reveny` oracle, it informs the
  read-only signal checklist in `selftest.sh`. **No code embedded or linked.**
  *Status: adopted as a test/checklist oracle; the native coverage of some of
  those signals is a roadmap item.*

- **TrustDevice Android** — `trustdecision/trustdevice-android` (**MIT**).
  Second **detector oracle / checklist** — a real device-risk SDK whose signal
  taxonomy is used the same way as FingerprintJS: as a black-box list of "what
  gets read," never as a source of copied code. *Status: adopted as a checklist
  oracle only.*

- **Thales device-fingerprint SDK (documentation)** — vendor/proprietary docs.
  The documentation was **unreachable at analysis time and is therefore
  unverified**; it is listed only to record that a commercial fingerprinting SDK
  was considered as a signal reference. **No technique, fact, or code has been
  adopted from it** pending verification against a reachable primary source.
  *Status: unverified; nothing adopted.*

---

## Phase 3 hooking-completeness audit (2026-08)

The Phase 3 review of `jni/` audited every `android.os.Build[.VERSION]` field
against the AOSP source to catch identifiers the SDK reads that our L1 hook did
not previously spoof. These are **factual references** — API type + system
property name — none of AOSP's code is copied.

- **AOSP `frameworks/base/core/java/android/os/Build.java`** (main branch,
  Apache-2.0):
  <https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/java/android/os/Build.java>
  Consulted for the exact field type (`String` / `String[]` / `int` / `long`)
  and the system property each field is initialized from — necessary to know
  that `SUPPORTED_ABIS` is `String[]` (not `String`), that `SKU` reads
  `ro.boot.hardware.sku`, that `VERSION.PREVIEW_SDK_INT` is `int`, etc. Result
  of the audit: `install_build_hook()` now spoofs `SERIAL`, `SUPPORTED_ABIS`,
  `SUPPORTED_32_BIT_ABIS`, `SUPPORTED_64_BIT_ABIS`, `CPU_ABI`, `CPU_ABI2`,
  `SKU`, `ODM_SKU`, `RELEASE_OR_CODENAME`, `RELEASE_OR_PREVIEW_DISPLAY`,
  `PREVIEW_SDK_FINGERPRINT`, `VERSION.BASE_OS`, `VERSION.PREVIEW_SDK_INT`,
  `VERSION.MEDIA_PERFORMANCE_CLASS`, and matching sysprops are written by
  `apply_native()` + `generate_mount_files()`.
  Follow-up audit (2026-08, Phase 4) additionally verified `Build.TIME =
  getLong("ro.build.date.utc") * 1000` and `VERSION.CODENAME`/`all_codenames`
  ("REL" on production builds), closing the last un-spoofed `Build` fields.

- **AOSP `frameworks/base/core/java/android/os/SystemProperties.java`** (main
  branch, Apache-2.0):
  <https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/java/android/os/SystemProperties.java>
  Consulted to confirm the string-keyed `native_get` / `native_get_int` /
  `native_get_long` / `native_get_boolean` are `@FastNative` (so the
  `(JNIEnv*, jclass, …)` hook signature in `install_prop_hook()` /
  `install_leak_sensors()` is the correct calling convention), while the
  long-handle `native_get_*` overloads used by `SystemProperties.Handle` are
  `@CriticalNative` and deliberately **not** hooked.

- **Zygisk sample repo (public API contract)**:
  <https://github.com/topjohnwu/zygisk-module-sample/blob/master/module/jni/zygisk.hpp>
  Referenced only to confirm `hookJniNativeMethods`, `pltHookRegister`, and
  `pltHookCommit` semantics for the hardened L8/L9 error handling. Our copy of
  the header is fetched at build time and pinned by commit SHA + SHA256 in
  `build.sh` (see the header block there for the exact revision).

- **AOSP chokepoint libraries for `clock_gettime` PLT hook** (Apache-2.0):
  - `frameworks/native/libs/utils/SystemClock.cpp` — `libutils`.
  - `frameworks/base/core/jni/android_os_SystemClock.cpp` — `libandroid_runtime`.
  These filenames are **facts** used to justify which shared libraries the L8
  hook registers PLT overrides against. `libbase` (`boot_clock`) and
  `libcutils` (`android_get_uptime()`) were also evaluated but deliberately
  excluded — hooking them too re-created the hooked-vs-unhooked clock
  divergence that hangs target apps on a white loading screen (see CHANGELOG
  2026-08-28 and `c67ae88`). No code from any of them is copied into this repo.

---

## Licensing note

SandboxID stays [MIT](./LICENSE). GPL/AGPL-licensed projects (e.g.
PlayIntegrityFork, and the *sensitive props* concept) are credited for the
*ideas and documented commands* adopted here; their **source code is not
included**, so no copyleft obligation attaches to this repository. MIT-licensed
projects (reveny/Android-VBMeta-Fixer, Hide-My-Goldfish, ezme-nodebug,
Zygisk-Assistant) likewise contributed **technique and factual property/mount
lists, not copied source** — had any code been vendored, its copyright + MIT
notice would be retained inline at the copy site. Closed-source detectors
(`libreveny.so`) are used only as black-box test oracles, never reverse-engineered
or copied. If you redistribute, keep this file and the LICENSE intact.
