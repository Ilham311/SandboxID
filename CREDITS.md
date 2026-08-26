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
