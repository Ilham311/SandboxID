# COPG-parity, minus the gaming — v1.0.30 explainer

> A design + code walkthrough of the v1.0.30 change that ports the **non-gaming,
> stealth** half of [COPG](https://github.com/AlirezaParsi/COPG) into Ternak TT:
> per-app **timezone**, **locale/language**, **SIM carrier**, **SoC** identity,
> and an opt-in **fake uptime**.

---

## Background

### What Ternak TT is

Ternak TT is a **Zygisk** module. Zygisk lets native code run inside the Android
app-zygote fork sequence, so a module can act *before and after* an app process
is specialized from the zygote but *before* the app's own code runs. Ternak TT
uses this window to make a small whitelist of apps (TikTok's
`com.zhiliaoapp.musically` / `com.ss.android.ugc.trill` and Grab's
`com.grabtaxi.passenger`, editable in `target.txt`) believe they are running on a
brand-new Google Pixel that has never seen those apps before — a "fresh persona".

The module is deliberately conservative. Its own `module.prop` says it
*"only changes safe identity strings, so it avoids risky hardware or framework
changes that can break boot."* That single sentence is the north star for this
change.

### How a value gets spoofed today

There are three cooperating layers. Using `Build.MODEL` as the worked example:

1. **The persona file.** `bin/ternak-tt freshen` generates a random Pixel and
   writes `/data/adb/modules/ternak_tt/identity.prop`, e.g. `MODEL=Pixel 8`.
2. **The companion → app handshake.** When a *target* app spawns, the Zygisk
   module (`jni/main.cpp`) asks the root **companion** (`jni/companion.cpp`) for
   the identity. The companion checks the package against `target.txt`, and if it
   matches, ships the whole `identity.prop` back as a text blob. Non-targets get a
   zero-length reply and the module unloads itself from them.
3. **The in-process hooks.** In `postAppSpecialize`, the module parses that blob
   into a `std::map` called `g_id`, then installs hooks:
   - `install_build_hook` overwrites the static `android.os.Build` fields
     (`MODEL`, `BRAND`, …) with `SetStaticObjectField`.
   - a `RegisterNatives` hook on `android.os.SystemProperties.native_get`
     intercepts `SystemProperties.get("ro.product.model")` and returns the persona
     value from `g_id`.

Separately, the root CLI also sets the corresponding `ro.*` system properties
device-wide via `resetprop-rs`, and bind-mounts a rewritten `build.prop` into the
target's **mount namespace** (so only that app sees it).

> [!NOTE]
> **Two hook classes.** COPG's README quietly splits its features into
> *stealth* (props, Build, Android ID, timezone, language — the module can unload,
> "anti-cheat safe") and *resident* inline hooks (GPU, refresh-rate, IMEI,
> aggressive-SIM, fake-uptime — "use at your own risk, never for anti-cheat
> games"). Ternak TT has only ever lived in the **stealth** class. That is the
> line this change refuses to cross.

### What COPG offers that we don't

COPG is first and foremost a **gaming** unlocker (120 FPS, HD graphics). Those
features — GPU renderer spoof, display refresh-rate spoof, CPU-flagship
`/proc/cpuinfo` spoof, DRM/Widevine level — are exactly what the task asked us to
skip, and they also happen to be COPG's *resident inline hooks*. What is left,
once you remove gaming, is a set of **identity** spoofs that fit Ternak TT's
market (fresh persona for TikTok/Grab) perfectly:

| COPG feature | Fits TT? | This PR |
|---|---|---|
| Timezone | ✅ stealth, identity | **Added** |
| Language / locale | ✅ stealth, identity | **Added** |
| SIM carrier (safe mode) | ✅ already prop-based | **Added** (persona-driven) |
| Extra Build / SoC fields | ✅ safe strings | **Added** (SoC) |
| Fake uptime | ⚠️ resident-ish but monotonic-safe | **Added, opt-in, default off** |
| Prop / Build / Android ID / GAID | ✅ | already in TT |
| GPU · refresh-rate · CPU-flagship · DRM | ❌ gaming | skipped |
| IMEI / Global IMEI · App Set ID · WebView UA · VPN-hide · mock-hide · hide-dev-options | ❌ need resident/inline hooks | skipped (see *Alternatives*) |

---

## Intuition

The key realization is that **timezone and locale never needed a new mechanism** —
they needed to be treated as *process defaults* instead of device settings.

Picture the Grab app starting up. Deep in its startup it calls, roughly:

```java
TimeZone tz = TimeZone.getDefault();          // -> "America/Los_Angeles"?
Locale   lc = Locale.getDefault();            // -> "en-US"?
String   op = tmgr.getNetworkOperatorName();  // -> "T-Mobile"?
```

If our fresh Pixel persona claims to be an Indonesian device but the process
answers `America/Los_Angeles` / `en-US` / `T-Mobile`, that mismatch is a louder
fingerprint than any single value. A *coherent* persona says the same thing
everywhere: `Asia/Jakarta`, `id-ID`, `Telkomsel`.

The elegant part is that Java keeps timezone and locale as **per-process static
defaults**. `TimeZone.setDefault(x)` changes what *this* process sees and nothing
else. So the "hook" is not a hook at all — we simply call the setter once, in the
app process, right after we've already loaded the persona:

```
freshen  ->  identity.prop:  TIMEZONE=Asia/Jakarta, LOCALE=id-ID
                                   │
target app spawns  ──▶ companion ships blob ──▶ g_id = { TIMEZONE:…, LOCALE:… }
                                   │
postAppSpecialize:  TimeZone.setDefault(getTimeZone("Asia/Jakarta"))
                    Locale.setDefault(forLanguageTag("id-ID"))
                    setenv("TZ", "Asia/Jakarta"); tzset();   // native/C side too
```

Nothing device-wide changes. The user's phone clock and system language are
untouched; only the four target apps see the persona region.

**Carrier** is even cheaper: Ternak TT *already* answered `gsm.operator.alpha`
and friends through the `native_get` hook — it just returned a hard-coded
`Telkomsel`. We turn those constants into persona fields so `freshen` can pick a
random-but-consistent Indonesian carrier.

**Fake uptime** is the one value that isn't a string. Apps read it via
`SystemClock.elapsedRealtime()` (and two siblings), which are `native` methods.
Because they're native, we can use the exact same `RegisterNatives` trick the
module already uses for `SystemProperties.native_get`. We re-derive the real value
from `clock_gettime` and add a **constant** offset:

```
elapsedRealtime()  =  CLOCK_BOOTTIME (ms)  +  FAKE_UPTIME_MS
```

Because the offset is constant, the clock still only moves forward and any
*duration* the app measures (`t2 - t1`) is unchanged — so timers and animations
behave normally, but "how long has this phone been on?" now reads days instead of
minutes.

> [!IMPORTANT]
> We never touch `System.currentTimeMillis()`. Wall-clock time is load-bearing
> for TLS certificate validity windows and Kerberos-style token freshness;
> shifting it would break HTTPS. Fake uptime shifts *uptime*, not *time of day*.

---

## Code

### 1. The persona now carries region + SoC fields

`jni/pool_tt.hpp` gains a `soc` per Pixel (correct Tensor generation) and a new
Indonesian carrier pool:

```cpp
struct CarrierEntry { const char* name; const char* mccmnc; const char* iso; };
static constexpr CarrierEntry TT_CARRIERS[] = {
    {"Telkomsel", "51010", "id"}, {"IND Indosat", "51001", "id"},
    {"XL Axiata", "51011", "id"}, {"3", "51089", "id"},
    {"SMARTFREN", "51009", "id"}, {"axis", "51008", "id"},
};
```

`gen_identity()` in `jni/ternak-tt.cpp` fills the new keys (SoC is truthful per
device; region defaults to the ID market; a carrier is chosen at random):

```cpp
id.kv["SOC_MANUFACTURER"] = "Google";
id.kv["SOC_MODEL"]        = p.soc;          // e.g. "Tensor G3" for a Pixel 8
id.kv["TIMEZONE"]         = "Asia/Jakarta";
id.kv["LOCALE"]           = "id-ID";
id.kv["LOCALE_LANG"]      = "id";
id.kv["LOCALE_COUNTRY"]   = "ID";
const CarrierEntry& c = TT_CARRIERS[g() % NC];
id.kv["GSM_OPERATOR_ALPHA"]   = c.name;
id.kv["GSM_OPERATOR_NUMERIC"] = c.mccmnc;
id.kv["GSM_OPERATOR_ISO"]     = c.iso;
```

All of these are added to `Identity::serialize()`'s fixed key order so they land
in `identity.prop` and get shipped in the companion blob.

### 2. The app-side hooks

`jni/main.cpp` gains two small installers, both called from
`postAppSpecialize` *after* the persona blob is parsed:

```cpp
static void install_locale_hook(JNIEnv* env) {
    // TimeZone.setDefault(getTimeZone(TIMEZONE)) + setenv("TZ") + tzset()
    // Locale.setDefault(forLanguageTag(LOCALE))
}

static void install_uptime_hook(JNIEnv* env) {
    if (val("FAKE_UPTIME_MS").empty()) return;   // opt-in
    // RegisterNatives on SystemClock.{uptimeMillis,elapsedRealtime,elapsedRealtimeNanos}
}
```

The uptime replacements re-implement the platform readers and add the offset:

```cpp
static jlong tt_sysclock_elapsed_realtime(JNIEnv*, jclass) {
    struct timespec ts; clock_gettime(CLOCK_BOOTTIME, &ts);
    return (jlong)(ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL) + g_uptime_offset_ms;
}
```

The `native_get` prop map and the `Build` field hook are extended for the new
surfaces: `persist.sys.timezone → TIMEZONE`, `persist.sys.locale` /
`ro.product.locale[.language|.region] → LOCALE*`, `ro.soc.* → SOC_*`, and
`Build.SOC_MANUFACTURER` / `Build.SOC_MODEL` (set only if the field exists —
API 31+ — otherwise the JNI exception is cleared and it's a no-op).

### 3. Device-wide vs app-scoped — a deliberate split

> [!WARNING]
> **Locale is never applied device-wide.** Writing `persist.sys.locale` with
> `resetprop` would reconfigure the *whole phone's* UI language on the next config
> reload. Instead locale lives only in (a) the per-app `build.prop` overlay that is
> bind-mounted into that one app's mount namespace, and (b) the in-process
> `Locale.setDefault`. `apply_native` (which sets props globally) only gained the
> two `ro.soc.*` entries, which are harmless informational strings.

### 4. Runtime edits: `ternak-tt set`

So the WebUI can change region without a reboot or a full re-`freshen`, the CLI
gets a `set` subcommand. It is an **allowlisted, line-preserving upsert**:

```cpp
static bool is_settable_key(const std::string& k) {
    // TIMEZONE, LOCALE, LOCALE_LANG, LOCALE_COUNTRY,
    // GSM_OPERATOR_ALPHA/_NUMERIC/_ISO, FAKE_UPTIME_MS  — nothing else
}
```

Two design choices worth calling out:

- It rewrites `identity.prop` **line by line** rather than through
  `Identity::serialize()`, because `serialize()` only emits keys in its fixed
  order and would silently drop shell-owned keys like `WIFI_MAC` /
  `BLUETOOTH_ADDR` that `rotate_ids.sh` appends. The line-based upsert preserves
  them.
- It refuses to set core device keys (`MODEL`, `FINGERPRINT`, …). Those belong to
  `freshen` as an internally-consistent set; poking one by hand would let the
  persona drift (a Pixel 8 body with a Pixel 6 fingerprint).

### 5. WebUI "Region" tab

`webroot/` gains a **Region** tab with inputs for timezone, locale, carrier
name / MCC+MNC / ISO, and fake uptime. **Save** chains one
`ternak-tt set KEY VALUE` per field (values single-quoted, so carrier names with
spaces survive), then tells the user to reopen the target app. The Persona tab now
also surfaces SoC, timezone, locale, carrier and fake-uptime.

---

## Verification

### What the agent verified

> [!NOTE]
> A full on-device Zygisk build needs the Android NDK, and the NDK mirror was
> **network-blocked in the build sandbox** (`dl.google.com` returned HTTP 403), so
> the agent could not produce a flashable `.zip` or run it on a phone. Verification
> was therefore compile-and-logic level. The GitHub Actions **Build & Release**
> workflow performs the real NDK cross-compile for all four ABIs on push.

- **CLI host build + functional test.** `jni/ternak-tt.cpp` is pure POSIX/C++ and
  was compiled and run on the host. `freshen` produced a persona with the new
  `SOC_MODEL=Tensor` (for a Pixel 6), `TIMEZONE=Asia/Jakarta`, `LOCALE=id-ID`,
  `CARRIER=Telkomsel (51010 / id)`, serialized in the correct key order.
- **`set` behaviour.** Verified that `set TIMEZONE`, `set LOCALE`, `set
  FAKE_UPTIME_MS` upsert correctly, that a pre-existing shell-owned `WIFI_MAC` line
  survived the rewrite, and that `set MODEL …` (not allowlisted) and
  `set FAKE_UPTIME_MS abc` (non-numeric) are both rejected with a non-zero exit.
- **Zygisk TU compile.** `jni/main.cpp` and `jni/companion.cpp` were syntax-checked
  in **both** the release and `TT_DEBUG` variants against stub `jni.h` /
  `android/log.h` / `zygisk.hpp` headers that mirror the exact JNI surface used
  (methods, signatures, `RegisterNatives` shape). The new JNI method signatures
  (`getTimeZone (Ljava/lang/String;)Ljava/util/TimeZone;`,
  `Locale.forLanguageTag`, `SystemClock.*()J`) are the standard platform ones.

### Manual QA checklist (on a rooted device)

1. Flash `ternak-tt-v1.0.30-debug.zip` via KernelSU/APatch/Magisk+ZygiskNext, reboot.
2. Tap **Action** (or WebUI → **Freshen persona**). Confirm the output now lists
   `SOC`, `TIMEZONE`, `LOCALE`, `CARRIER`.
3. In a target app (e.g. a device-info app added to `target.txt`), confirm it
   reads the persona timezone, language and carrier; confirm a **non-target** app
   still shows your real values.
4. WebUI → **Region** → change timezone to `Europe/London`, Save, reopen the
   target app, confirm it now reads London time. Confirm the phone's own clock is
   unchanged.
5. WebUI → **Region** → set fake uptime to `864000000` (10 days), Save, reopen the
   target app, confirm "uptime" reads ~10 days while a stopwatch inside the app
   still measures real elapsed durations. Set back to `0` to disable.
6. Reboot once more and confirm no bootloop (the safety-critical check).

---

## Alternatives

### A. Full per-app profiles + package tags (the literal COPG model)

COPG stores a `COPG.json` with per-package tag suffixes (`:tz=`, `:lang=`,
`:sim=`, …) so *each* app can have a *different* timezone/locale/carrier.

| Pros | Cons |
|---|---|
| Maximum flexibility; matches COPG 1:1 | A large redesign of TT's single-persona model |
| Different persona per app | The whole point of TT is *one* coherent fresh persona for a tiny whitelist |
| — | New config format, parser, and migration; much bigger surface to get wrong |

Rejected: it fights Ternak TT's core "one fresh persona" design for no benefit to
the TikTok/Grab use case. The chosen design reuses the existing `identity.prop` +
companion-blob pipeline unchanged.

### B. Add a real inline-hook engine (Dobby / ShadowHook) for IMEI, VPN-hide, etc.

Would unlock the *resident* COPG features (IMEI, App Set ID, VPN-hide,
hide-developer-options) that can't be done with `RegisterNatives` (those are
non-`native` Java methods reached over Binder).

| Pros | Cons |
|---|---|
| Unlocks the last COPG identity features | A new native dependency + PLT/inline patching |
| True IMEI / VPN-hide parity | **Directly contradicts** the module's "no risky changes that can break boot" charter |
| — | Un-testable here (no device); high bootloop risk to ship blind |
| — | These are COPG's "use at your own risk / never anti-cheat" hooks — the opposite of TT's stealth stance |

Rejected for this PR: it crosses the stealth/resident line the module has always
respected. Documented here so a future maintainer can make that call deliberately.

---

## Suggested people to talk to

- **Ilham (`@Ilham311`, `diru768@gmail.com`)** — the maintainer and primary author
  of the Zygisk hook layer (`jni/main.cpp`) and the persona/companion pipeline
  (`jni/ternak-tt.cpp`, `jni/companion.cpp`). Best person to sanity-check the new
  `install_locale_hook` / `install_uptime_hook` placement in `postAppSpecialize`
  and whether the ID-market defaults (Asia/Jakarta, id-ID, Telkomsel pool) are the
  right fit for the target apps.

---

## Quiz

<details>
<summary><b>1. Why is <code>System.currentTimeMillis()</code> deliberately left un-spoofed by the fake-uptime feature?</b></summary>

- **A.** It isn't a native method, so `RegisterNatives` can't reach it.
- **B.** ✅ Wall-clock time is used to validate TLS certificate validity windows; shifting it would break HTTPS. Fake uptime shifts *uptime* (`elapsedRealtime`/`uptimeMillis`), not time-of-day.
- **C.** It's already covered by the `native_get` property hook.
- **D.** Apps never call it.

*A is wrong (it happens to be native-backed too); C/D are false. The reason is correctness/safety of wall-clock time.*
</details>

<details>
<summary><b>2. Why does <code>cmd_set</code> rewrite <code>identity.prop</code> line-by-line instead of calling <code>Identity::serialize()</code>?</b></summary>

- **A.** `serialize()` is slower.
- **B.** `serialize()` only emits keys in its fixed order list and would drop shell-owned keys like `WIFI_MAC` / `BLUETOOTH_ADDR` that `rotate_ids.sh` appends. ✅
- **C.** `serialize()` requires root and `set` doesn't.
- **D.** To avoid an atomic write.

*B: the line-based upsert preserves every unknown/shell-owned key; serialize would silently discard them.*
</details>

<details>
<summary><b>3. Why is locale applied via the in-process <code>Locale.setDefault</code> and the app-scoped <code>build.prop</code> overlay, but NOT via device-wide <code>resetprop persist.sys.locale</code>?</b></summary>

- **A.** `resetprop` can't set `persist.*` properties.
- **B.** Device-wide `persist.sys.locale` would reconfigure the whole phone's UI language on the next config reload — a real, user-visible device change. ✅
- **C.** The persona file can't store a locale.
- **D.** Locale isn't read by apps.

*B: the module's charter is to change only what the target app sees, never the real device. Timezone/locale/carrier are kept strictly per-app.*
</details>

<details>
<summary><b>4. A fresh <code>freshen</code> picks a Pixel 8. Which SoC value does the persona report, and where?</b></summary>

- **A.** `Snapdragon 8 Gen 3`, via `/proc/cpuinfo`.
- **B.** `Tensor G3`, via `Build.SOC_MODEL` (API 31+) and `ro.soc.model`. ✅
- **C.** Nothing — SoC isn't spoofed.
- **D.** `Tensor`, via `ro.hardware`.

*B: `pool_tt.hpp` maps each Pixel to its real Tensor generation (Pixel 8 → Tensor G3); it is exposed through the Build static field and the `ro.soc.*` prop hook. There is no `/proc/cpuinfo` spoof (that's the skipped gaming CPU feature).*
</details>

<details>
<summary><b>5. Why can timezone/locale be done with a plain setter call, but IMEI spoofing was NOT implemented in this PR?</b></summary>

- **A.** IMEI is a `String` and timezone is an `int`.
- **B.** Timezone/locale are per-process **static defaults** you can set directly; IMEI is served by the telephony system service over Binder and its getters are non-`native` Java methods, which `RegisterNatives` cannot replace — it needs a resident inline-hook engine the module deliberately avoids. ✅
- **C.** IMEI is stored in `identity.prop` already.
- **D.** Timezone requires root and IMEI doesn't.

*B: `TimeZone.setDefault` / `Locale.setDefault` mutate process-local state; there is no equivalent "setter" for the value telephony returns, so IMEI would require inline hooking (out of scope, see Alternatives B).*
</details>
