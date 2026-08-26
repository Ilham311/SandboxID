# Parse-token safelist — the shell↔WebUI output contract

`webroot/app.js` is not a passive log viewer. It **parses the stdout** of
`action.sh` and `rotate_ids.sh` with regexes and reads `identity.prop` by key.
If a PR changes an output string, a log prefix, or a prop key **without**
changing the matching pattern in `app.js`, the WebUI silently mis-reports
success/failure or drops a field — with no error. This is the single most
common way a "cosmetic" copy edit breaks the product.

**Reviewer rule:** any diff that touches a line below on *one* side of the
contract must touch the *other* side in the same PR, or justify why the match
still holds. Treat an unpaired change as **Important** (silent WebUI breakage),
or **Critical** if it flips a failure into a reported success.

## 1. Action summary — `summarizeAction()` (`webroot/app.js:186`)

| Pattern in app.js | Produced by | Meaning |
|---|---|---|
| `/^OK - persona baru aktif/m` | `action.sh:124` `say "OK - persona baru aktif"` | multibrand success banner |
| `/^OK - fresh/m` | native `sandboxid freshen` stdout (backup path, `action.sh:71`) | freshen success banner |
| `/^\s*BRAND\s*:\s*(.+)$/m` | `action.sh:125` `  BRAND       : $_brand` | hero brand label |
| `/^\s*MODEL\s*:\s*(.+)$/m` | `action.sh:126` `  MODEL       : $_mkt ($_model)` | hero model label |
| `/^(?:Gagal\b\|[\u2717!]).*$/m` | any line starting with `Gagal`, `✗` (U+2717), or `!` | failure line → error toast |

Keep the literal prefixes `OK - persona baru aktif`, `OK - fresh`, the
`KEY :` two-space-colon shape for `BRAND`/`MODEL`, and the `Gagal`/`✗`/`!`
failure markers. Renaming "Gagal" to "Error" or dropping the `OK - ` prefix
breaks success/failure classification.

## 2. Rotate summary — `summarizeRotate()` (`webroot/app.js:204`)

| Pattern in app.js | Must appear in `rotate_ids.sh` output |
|---|---|
| `/\[ERR\]/g` | error lines tagged `[ERR]` |
| `/\[WARN\]/g` | warning lines tagged `[WARN]` |
| `/(\d+) step\(s\) reported failure/` | the exact phrase `N step(s) reported failure` |
| `/REBOOT REQUIRED/i` | the literal `REBOOT REQUIRED` marker |

## 3. Log line classification — `classifyLine()` (`webroot/app.js:157`)

Line-level colouring in the Log tab keys off these prefixes:

- `==>` → step, `[OK]` / `OK ` → ok, `[WARN]` → warn, `[ERR]` / `!` → err
- logcat lines: `/^\d\d-\d\d \d\d:\d\d:\d\d\.\d+\s+([VDIWEF])\//` (standard
  `-v time` logcat format; `E`/`F`→err, `W`→warn). Do not reformat logcat
  invocations away from `-v time` (`webroot/app.js:446`).

## 4. `identity.prop` key vocabulary — `parseProp()` + consumers

The WebUI reads `identity.prop` as `KEY=VALUE` and looks up these exact keys.
Renaming a key in a writer without updating `app.js` drops the field from the UI
silently. Writers: `autopif.sh device` (builds `device.identity`, copied to
`identity.prop` at `action.sh:52` — the primary path and the *sole* producer of
`BOOT_COUNT`, `UPTIME_HUMAN`, `UPTIME_SECONDS`, `FRESH`, `USAGE_PROFILE`,
`FIRST_BOOT`, `LAST_BOOT`), native `sandboxid freshen`, `rotate_ids.sh`, and
`helpers.sh identity_persist`.

- **Hero** (`renderHero`, `app.js:284`): `BRAND`, `MARKETNAME`, `MODEL`,
  `DEVICE`, `RELEASE`, `SDK_INT`, `FINGERPRINT`
- **Tiles** (`renderTiles`, `app.js:299`): `BOOT_COUNT`, `UPTIME_HUMAN`,
  `UPTIME_SECONDS`, `FRESH` (truthy match `/^(y|yes|true|1)$/i`),
  `USAGE_PROFILE`
- **Detail grid** (`DETAIL_KEYS`, `app.js:275`): `MANUFACTURER`, `PRODUCT`,
  `BOARD`, `SOC_MANUFACTURER`, `SOC_MODEL`, `SECURITY_PATCH`, `SERIAL`,
  `ANDROID_ID`, `GOOGLE_AID`, `WIFI_MAC`, `BLUETOOTH_ADDR`, `BLUETOOTH_NAME`,
  `RADIO`, `FIRST_BOOT`, `LAST_BOOT`
- **Rotate cards** (`ROT_CARDS`, `app.js:358`) read: `ANDROID_ID`,
  `GOOGLE_AID`, `WIFI_MAC`, `BLUETOOTH_ADDR`, `MODEL`, `BOOT_COUNT`

## 5. `module.prop` shape

`app.js:477` extracts the version with `sed -n 's/^version=//p'`. Root managers
(KernelSU/Magisk/APatch) parse `id`, `name`, `version`, `versionCode`,
`author`, `description`, `updateJson`. Keep these as bare `key=value` lines with
no surrounding quotes or reordering that a parser might choke on. `id` must stay
`sandboxid` (lowercase) — it is the on-device module path
`/data/adb/modules/sandboxid` hardcoded in `app.js:3` and every script's
`MODDIR`.

## 6. Self-check grammar — `parseSelftest()` (`webroot/app.js`)

The **Uji** tab runs `selftest.sh` and parses its stdout. `selftest.sh` is
read-only and prints ONLY these two line shapes; `app.js` matches them exactly:

| Pattern in app.js | Produced by `selftest.sh` | Meaning |
|---|---|---|
| `/^SELFTEST\s+(\S+)\s+(PASS\|WARN\|FAIL\|INFO)\s*(.*)$/` | every `emit` line | one check → result card |
| `/^SELFTEST\s+SUMMARY\s+(.*)$/` then `/(\w+)=(\d+)/g` | the final `SELFTEST SUMMARY pass=N warn=M fail=K info=J` | summary chip |

Contract rules:

- The leading literal token `SELFTEST` and the four status words
  `PASS`/`WARN`/`FAIL`/`INFO` are fixed. Renaming any of them, or translating
  them (e.g. to `LULUS`), silently drops every card.
- `<category>` is a single **space-free** token. `app.js` maps known tokens
  (`identitas`, `vbmeta`, `build`, `selinux`, `rom`, `root`, `mount`, `hosts`,
  `hooks`) to friendly labels in `ST_CAT` and falls back to the raw token — so a
  new category still renders, just unlabelled until added to `ST_CAT`.
- The summary keys parsed are `pass`/`warn`/`fail`/`info` via a generic
  `key=number` scan, so extra keys are harmless and order-independent, but
  renaming these four blanks that field in the chip.
- **Semantics that must not drift:** per-app effects (property hooks L2/L9,
  `/proc`,`/sys` read redirects, `build.prop` binds, the opt-in mount-trace
  hider) and device-global root traces (su, manager dirs, mounts) are reported
  `INFO`, never `FAIL` — a root shell cannot see inside a target app's hook/mount
  namespace, so grading them FAIL would be a false negative for the user. Only
  device-wide, root-shell-visible properties are graded PASS/WARN/FAIL.
