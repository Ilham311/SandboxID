# Credits & References

SandboxID is [MIT](./LICENSE) licensed. It is built on **publicly documented
platform commands and techniques**. Where an approach was learned from another
open-source module, that project is credited below. **No third-party source
code is copied into this repository** — only documented, idiomatic one-line
commands (`pm clear`, `am force-stop`, `killall`, `resetprop -n`) and the
general technique are adopted, so SandboxID remains MIT and the referenced
projects retain their own licenses.

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

---

## Licensing note

SandboxID stays [MIT](./LICENSE). GPL-licensed projects (e.g. PlayIntegrityFork)
are credited for the *ideas and documented commands* adopted here; their source
code is **not** included, so no copyleft obligation attaches to this repository.
If you redistribute, keep this file and the LICENSE intact.
