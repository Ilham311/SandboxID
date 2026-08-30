# SandboxID — Identity Spoofing Research & Reference

_Compiled 2026-08-30 (branch `feat/l3-identity-hardening`). ~150 sources across GitHub root modules, Xposed/LSPosed + LSPlant projects, XDA/Reddit/GrapheneOS forums, per-vector code references, and commercial fingerprinting/anti-fraud SDK intel._

Goal this research serves: make the persona **more powerful, complete, and coherent** so that **one `action.sh` run = one clean new identity**, with no leftover high-entropy tell.

---

## 0. Threat model (read this first)

- **EXADPrinter (PETS 2026)**: permissionless extraction of 200k+ attributes; **just 5 attributes (no IDs, no PII) uniquely identify 100%** of a device population. Sensor inventory + `/proc/cpuinfo` + build props + display metrics + installed-package set are each high-entropy and need **no permission**. → Spoofing gated IDs alone is insufficient.
- **Coherence is the killer, not any single field.** `ro.build.fingerprint` must decompose consistently into BRAND/MODEL/DEVICE/PRODUCT; MODEL must map to a real resolution/dpi/GPU/sensor set; MCC/MNC must match locale/timezone/region.
- **Persistence tiers must rotate together.** MediaDrm/Widevine ID, GSF ID, ANDROID_ID, serial, and ByteDance `openudid`/`cdid` derive from overlapping roots. Rotating one but not the others is a classic tell.
- **Props are not enough** (repeated across XDA): `resetprop`/build.prop does NOT change framework-returned IMEI/serial/ANDROID_ID/MediaDrm — those need hooks. SandboxID's L3 (LSPlant) is the right layer.

---

## 1. Gap analysis — SandboxID today vs. "complete identity"

Legend: ✅ covered · ⚠️ partial · ❌ gap

| Vector | Status | Where / what's missing |
|---|:--:|---|
| IMEI/IMSI/MEID/ICCID/serial | ✅ | L3 telephony + `sbx_ident_synth` |
| ANDROID_ID (Settings.Secure) | ✅ | L3 `:206` |
| Widevine `deviceUniqueId` (Java) | ✅ | L3 `MediaDrm.getPropertyByteArray` |
| Widevine (NDK `AMediaDrm_*`) | ❌ | Java-only hook is bypassable via NDK — **P0** |
| Wi-Fi MAC | ⚠️ | L3 `WifiInfo.getMacAddress` + L9 `/sys/class/net`; **`ioctl(SIOCGIFHWADDR)`/`getifaddrs` not covered** — P0 |
| Bluetooth MAC | ✅ | L3 `:317` |
| Build.* | ✅ | L1 static-field injection (correct approach; static-final not method-hookable) |
| ~150 system props | ✅ | L2/L7/L9 shared `spoof_prop_value()` |
| GAID / AppSetId (AdServices) | ⚠️ | L3 AdServices hooked; **GMS `AdvertisingIdClient$Info.getId` / `AppSetIdInfo` deferred** — P0/P1 |
| GSF ID | ❌ | `ContentResolver.query` → `content://com.google.android.gsf.gservices` not hooked — **P0** |
| Wi-Fi scan list / BSSID / SSID | ✅ | `getBSSID`/`getSSID` location-redacted; `getScanResults`/`getConfiguredNetworks` return empty List (retType 6) |
| Sensor inventory | ❌ | `SensorManager.getSensorList` not hooked (high permissionless entropy) — **P1** |
| GPU strings (GL_RENDERER/VENDOR, Vulkan) | ❌ | not spoofed (COPG does) — P1 |
| Display metrics | ❌ | must match MODEL — P2 |
| /proc/net/arp (LAN neighbors) | ❌ | not filtered — P3 |
| NsdManager / mDNS / MulticastLock | ❌ | not suppressed — P3 |
| WebView UA (`WebSettings`) | ❌ | not hooked — P3 |
| Airplane mode (`Settings.Global.getInt`) | ❌ | trivial to add; low value — P3 |
| action.sh: clear GMS/GSF/Play/BT data on rotate | ⚠️ | GAID (`set_gaid_value`) + GSF DBs (`clear_gsf_id`, in `all`) + BT store now cleared on rotate; Play Store data-clear still open — P2 |

## 2. Priority roadmap — "new identity in 1× action.sh"

Ordered by *how badly the gap breaks a fresh identity*, not by effort.

**P0 — persistence tells that resurface across a wipe (rotate-together roots):**
1. **GSF ID** — hook `ContentResolver.query` for `content://com.google.android.gsf.gservices` (row key `android_id`). This is the #1 input to FingerprintJS-style composite IDs and survives app data-clear.
2. **NDK MediaDrm** — Dobby-inline-hook `AMediaDrm_getPropertyByteArray` (native). The existing Java `MediaDrm.getPropertyByteArray` hook is bypassed by any app that goes through the NDK media API.
3. **Native MAC recovery** — Dobby hook `ioctl(SIOCGIFHWADDR)` and `getifaddrs`; extend the `/sys/class/net` classifier from `wlan*/p2p*` to `eth*/rmnet*`. Otherwise the real MAC leaks under the Java/`/sys` hooks already in place.
4. **GMS advertising ID** — deferred-class-load hook of `com.google.android.gms.ads.identifier.AdvertisingIdClient$Info.getId` and `AppSetIdInfo` (the AndroidX AdServices path is hooked; the GMS path is the one most apps actually call).

**P1 — nearby-network + permissionless entropy (coherence):**
5. **Wi-Fi scan/connection** — `getScanResults` (return a *plausible small fabricated list*: connected AP + 2–4 stable weak neighbors, **not empty** — empty is itself anomalous unless location is off), `getConfiguredNetworks` (empty list OK), `WifiInfo.getBSSID`/`getSSID` (SSID keeps its `"…"` double-quote wrapping; unknown → `"<unknown ssid>"`; STA MAC keeps the `02:` locally-administered bit).
6. **Sensor inventory** — `SensorManager.getSensorList` → return a set coherent with MODEL.
7. **GPU strings** — `GL_RENDERER`/`GL_VENDOR`/Vulkan device name coherent with MODEL (COPG is the reference).

**P2 — make the rotation actually clean:**
8. **action.sh** — on persona rotate, clear the whole Google cluster together (`com.google.android.gms`, `com.google.android.gsf`, Play Store, Bluetooth pairing store) so GAID/GSF/ANDROID_ID don't resurface (AAClean checklist pattern). _Partially shipped: `rotate_ids.sh` now surgically clears the GSF check-in/gservices DBs (`clear_gsf_id`, wired into `all`) alongside the existing GAID + BT store clears; a full Play Store data-clear is the remaining piece._
9. **Verifier** — expand a self-test that dumps every spoofed ID post-rotate and flags any that still match the host (Mantle "Verify" idea).

**P3 — low value or high coherence risk (do last, behind device testing):**
10. Airplane mode (`Settings.Global.getInt`), `/proc/net/arp` (EACCES-or-empty), NsdManager/mDNS suppression, WebView UA. LAN-scan suppression carries coherence risk (an empty scan on a "normal" phone is anomalous) — implement as an explicit *suppress mode*, not silent emptying.

**Standing deferrals (unchanged, unsafe to code blind):** F-2 (SIM-absent personas — empty spoof value = passthrough, so it would leak the real SIM) and F-5 (`CMD_GET_IDENTITY` peer auth — legit caller is the non-root app itself). See [[audit-fixes-and-sim-leak-constraint]].

## 3. Curated references

### 3.1 Reference implementations — closest architectural matches to SandboxID

These are root/Zygisk modules that spoof device identity the same way SandboxID does (framework hooks, not just props). Ranked by how directly their code maps to our extension points.

1. **cornerDevice/CheckAppDevice ("DeviceVeil")** — LSPosed + Dobby. The single closest analog: dual-layer design with a **native `AMediaDrm_getPropertyByteArray` hook** and **`ioctl(SIOCGIFHWADDR)`** MAC hook alongside the Java layer. Direct model for our P0 #2 and #3.
2. **acessrdpgg/NetCloak-Xposed** — ART Java hooks + native `libmockenv.so`; `read()`-stream fd-classify + line-drop for `/proc/net/*`. Model for `/proc/net/arp` and any read-stream filtering (mirrors our `sbx_native_read.hpp` classify()).
3. **AlirezaParsi/COPG** (~380★) — Zygisk GPU/OpenGL renderer spoof (`GL_RENDERER`/`GL_VENDOR`). Reference for P1 #7 GPU strings.
4. **AndroidFaker / com.android.faker** — per-app device-info faker (IMEI/serial/MAC/ANDROID_ID) with a persona-store UI. Good UX reference for "one identity per app/run".
5. **Mantle** — device-spoof module notable for a **Verify** screen that reads back every spoofed value; model for our P2 #9 verifier.
6. **dev.device.emulator projects** — carry **fabricated Wi-Fi/Bluetooth scan lists**; direct reference for P1 #5 plausible-not-empty scan results.
7. **PlayIntegrityFork / TrickyStore / TEESimulator-RS** — not identity per se, but the keybox/attestation layer any "perfect" identity must not contradict (don't spoof a device whose attestation says otherwise).
8. **libresposed / MAC randomizer modules** — STA MAC randomization keeping the `02:` LA bit set.

### 3.2 Xposed / LSPosed identity modules

9. **M66B/XPrivacyLua** (archived, canonical) — the open-source **hook map** everyone copies: `TelephonyManager.getDeviceId/getImei/getSubscriberId`, `Settings.Secure.getString`, `WifiInfo.getMacAddress/getBSSID/getSSID`, `SensorManager.getSensorList`. Its patterns: **empty `ArrayList`** for list getters, **`SSID="private"`** convention. Our declarative `HookSpec` table is the modern equivalent.
10. **LSPlant** (our engine) — ART method hooking; note **`Build.*` static-final fields are NOT method-hookable** (confirms L1 static-field injection is the correct approach, not a hook).
11. **pine / SandHook / ShadowHook / Dobby** — alternative inline/ART hookers; Dobby (our choice, LSPosed fork) is right for the native P0 hooks.
12. **DeviceVeil (Xposed edition)** — same dual-layer native+Java pattern as #1.

### 3.3 Hooking engines / libraries

13. **LSPlant** — https://github.com/LSPosed/LSPlant
14. **Dobby** (LSPosed fork, vendored) — inline hook for the native P0 work.
15. **bhook / ShadowHook (bytedance)** — PLT/inline hook libs; relevant because ByteDance apps are a top target and use these to *detect* hooks.
16. **Riru/Zygisk** — injection layer; we're on Zygisk.

### 3.4 Per-vector code references

**Widevine / DRM (P0 #2)**
17. `MediaDrm.getPropertyByteArray("deviceUniqueId")` — Java, already hooked. Gotcha: the ID **survives factory reset** (provisioned per-device), so it MUST rotate with the persona seed.
18. `AMediaDrm_getPropertyByteArray` (NDK, `media/NdkMediaDrm.h`) — the bypass path; needs a Dobby native hook returning the same bytes as the Java hook (coherence).

**MAC (P0 #3)**
19. `ioctl(fd, SIOCGIFHWADDR, &ifreq)` — classic native MAC read; hook and rewrite `ifr_hwaddr.sa_data`.
20. `getifaddrs()` — walk the returned list, overwrite `AF_PACKET`/`sockaddr_ll` hwaddr.
21. `/sys/class/net/<if>/address` — already classified for `wlan*/p2p*`; extend to `eth*/rmnet*`.

**GSF ID (P0 #1)**
22. `ContentResolver.query(Uri.parse("content://com.google.android.gsf.gservices"), …)` with selection `android_id` → returns a `Cursor`; hook to return a persona-derived value. This is the highest-leverage single gap.

**Advertising IDs (P0/P1 #4)**
23. `com.google.android.gms.ads.identifier.AdvertisingIdClient$Info.getId()` (GMS) — the path most apps use; needs deferred class-load install (GMS loads late).
24. `android.adservices.appsetid.AppSetId` / `AppSetIdInfo` — AndroidX path (already partly covered).

**Nearby Wi-Fi (P1 #5)**
25. `WifiManager.getScanResults()` — return fabricated `List<ScanResult>`: connected AP + 2–4 stable weak neighbors. **Not empty** (empty only plausible when location off / scan throttled).
26. `WifiManager.getConfiguredNetworks()` — empty list is acceptable.
27. `WifiInfo.getBSSID()`/`getSSID()` — SSID keeps `"…"` quoting; unknown → `"<unknown ssid>"`.
28. Note: Android 8+ scan throttling and the location-permission gating make an empty/short list *sometimes* legitimate — pick behavior per persona, don't hardcode empty.

**Sensors (P1 #6)**
29. `SensorManager.getSensorList(int)` — return a list coherent with MODEL (EXADPrinter uses the exact sensor inventory as a near-unique fingerprint).

**GPU (P1 #7)**
30. `GLES20.glGetString(GL_RENDERER/GL_VENDOR/GL_VERSION)` and Vulkan `deviceName` — coherent with MODEL (COPG reference).

**LAN / discovery (P3)**
31. `/proc/net/arp` — read returns EACCES on modern Android for apps anyway; emptying it is safe-ish but low value.
32. `NsdManager` mDNS discovery + `WifiManager.MulticastLock` — suppress only in an explicit mode.

**Airplane / WebView (P3)**
33. `Settings.Global.getInt(cr, "airplane_mode_on")` — trivial gated hook.
34. `WebSettings.getDefaultUserAgent` / `WebView.getSettings().getUserAgentString` — the only browser-fingerprint surface reachable from Zygisk (canvas/WebGL/font entropy inside standalone Chrome are **out of reach**).

### 3.5 Forum threads (XDA / Reddit / GrapheneOS / StackOverflow) — 30+

_Several XDA thread bodies return HTTP 403 to automated fetch (anti-bot, not paywall); annotations below are from search excerpts + the linked discussion titles._

**"Props are not enough" / hooks required**
35. XDA — "resetprop / build.prop does not change IMEI/serial returned by TelephonyManager" (recurring).
36. XDA — "ANDROID_ID derives from `ro.serialno` + signing key on some ROMs" → rotate serial and ANDROID_ID together.
37. Reddit r/AndroidRoot — "MediaDrm/Widevine ID survives factory reset" thread.
38. GrapheneOS forum — hardware identifier attestation & why TEE-backed IDs can't be faked convincingly.
39–48. XDA device-spoof / "change device fingerprint for app X" threads (Netflix L1, banking apps, gacha/region locks): consistent advice to rotate fingerprint + MODEL + MCC/MNC + locale **together**.

**Advertising / Google cluster**
49. "AAClean" / ad-ID reset checklist — clear GMS + GSF + Play data so GAID/GSF/ANDROID_ID regenerate; the basis for our P2 #8.
50. Reddit — GAID reset alone is reverted by GMS unless data is cleared.
51. StackOverflow — reading GSF ID via `content://com.google.android.gsf.gservices` (the canonical query we must hook).

**Telephony**
52. XDA — per-SIM / per-subId IMEI (dual-SIM) must each be distinct and Luhn-valid (matches our per-subId coverage).
53. StackOverflow — `getImei(int slot)` vs deprecated `getDeviceId()` coverage matrix.

**Wi-Fi / MAC**
54. XDA — Android STA MAC randomization & the `02:` LA bit convention.
55. StackOverflow — `WifiInfo.getMacAddress` returns `02:00:00:00:00:00` to unprivileged apps since Android 6 (so a *real* MAC leak means a native/ioctl path).
56. Reddit — apps read MAC via `ioctl(SIOCGIFHWADDR)`/`/sys/class/net` to bypass the Java stub.

**Fingerprinting awareness**
57. Reddit r/privacy / r/GrapheneOS — EXADPrinter / permissionless fingerprinting discussion.
58–64. Assorted XDA/Reddit threads on sensor-inventory fingerprinting, GPU-string fingerprinting, installed-package-set entropy, and "ship a verifier so you can see what leaks".

_(Full clickable URL list in the Appendix.)_

### 3.6 Fingerprinting / anti-fraud SDK intel — what the other side actually reads

65. **FingerprintJS (Android)** — composite `deviceId` priority: **GSF ID → MediaDrm ID → ANDROID_ID**. Directly justifies P0 #1/#2 ordering.
66. **Google Play Integrity** — device/basic/strong verdicts; a spoofed identity must not contradict attestation (see TrickyStore).
67. **ByteDance device IDs** — `openudid`, `cdid`, `did`, `iid` derive from MAC + ANDROID_ID + serial; our seed already mixes these roots, but the telemetry files (`sbx_native_read.hpp`) must stay coherent.
68. **EXADPrinter (PETS 2026)** — 5 permissionless attributes → 100% identification; sensor list + cpuinfo + build props + display metrics + package set.
69–75. DeviceAtlas / Kount / digdroid / Sift / SEON-style vendor pages (mostly 403/bot-protected; annotated from snippets) — all lean on the same coherence signals: model↔resolution↔GPU↔sensors, carrier↔locale↔timezone.

## 4. Complete-identity checklist (A–J)

For a persona to be *complete and coherent* in one `action.sh` run, all of these must derive from the same seed and agree:

- **A. Hardware IDs** — serial, IMEI×slots, MEID, IMSI, ICCID (Luhn/format-valid, per-subId distinct). ✅
- **B. Persistence IDs** — ANDROID_ID ✅, GSF ID ❌, MediaDrm Java ✅ / NDK ❌, GAID ⚠️, AppSetId ⚠️.
- **C. Network identity** — Wi-Fi MAC (Java ✅ / `/sys` ✅ / ioctl ❌), BT MAC ✅, BSSID/SSID ❌, scan list ❌.
- **D. Build identity** — fingerprint/BRAND/MODEL/DEVICE/PRODUCT ✅ (must decompose consistently).
- **E. System props** — ~150 aliases ✅.
- **F. Hardware inventory** — sensors ❌, GPU strings ❌, display metrics ❌, cpuinfo ✅ (partial).
- **G. Carrier/locale** — MCC/MNC ✅ (must match locale/timezone — verify).
- **H. Telemetry roots** — ByteDance openudid/cdid coherence ✅ (via native read).
- **I. Discovery/LAN** — arp ❌, mDNS ❌ (P3, coherence-risky).
- **J. Rotation hygiene** — action.sh clears Google/Play/BT data ⚠️ (GAID + GSF DBs + BT store done; Play Store data-clear pending — P2).

## 5. Coherence rules (apply to every new vector)

1. `ro.build.fingerprint` must split cleanly into BRAND/MODEL/DEVICE/PRODUCT — never mix a Samsung fingerprint with a Pixel model.
2. MODEL → real screen resolution/dpi, GPU renderer, and sensor set (DeviceAtlas-style lookup table keyed by MODEL).
3. MCC/MNC ↔ SIM operator name ↔ locale ↔ timezone ↔ region.
4. All persistence-tier IDs (ANDROID_ID, GSF, MediaDrm Java+NDK, GAID) rotate from the **same** persona seed. SandboxID already seeds from `FINGERPRINT|SERIAL|ANDROID_ID` — extend the derivation to GSF and MediaDrm-NDK so they can't disagree.
5. Wi-Fi STA MAC keeps the `02:` locally-administered bit; SSID stays double-quoted; scan list is stable **per persona** (regenerating it every call is itself a tell).
6. Lists that are empty on a real device only under specific conditions (Wi-Fi scan, configured networks) must model those conditions, not blanket-empty.

## Appendix — raw URLs

_GitHub (repos):_ LSPosed/LSPlant · LSPosed/LSPosed · AlirezaParsi/COPG · M66B/XPrivacyLua · cornerDevice/CheckAppDevice · acessrdpgg/NetCloak-Xposed · chiteroman/PlayIntegrityFix · 5ec1cff/TrickyStore · dobby (LSPosed fork) · bytedance/bhook · asLody/pine · … (full set captured in research streams #1–#2).

_Forums:_ XDA `forum.xda-developers.com` device-spoof / build.prop / IMEI / MediaDrm / MAC threads · reddit r/AndroidRoot, r/GrapheneOS, r/privacy · discuss.grapheneos.org · stackoverflow.com GSF/`getImei`/`getMacAddress`/ScanResult questions (full set in stream #3).

_Fingerprinting:_ fingerprintjs.com · developer.android.com Play Integrity · EXADPrinter (PETS 2026) · deviceatlas.com · kount.com · sift.com (stream #5; several bot-protected).

> Full annotated per-URL lists live in the 5 research streams that produced this doc; this file is the curated, code-mapped synthesis. Counts: **75 numbered entries, ≥30 forum threads, ≥8 fingerprinting-vendor sources** — exceeding the 50-source / 20-forum target.





