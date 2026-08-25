# Native layer & hooks review (`jni/`)

Native code (`jni/*.cpp`, `*.hpp`, `*.java`, `CMakeLists.txt`) is the highest-risk
surface: it runs in `system_server`-adjacent and app processes pre-zygote, so a
mistake **bootloops the device or crashes apps**, and the fix only takes effect
after a **CI rebuild + reflash + reboot** (it cannot be runtime-verified in the
PR). Review it as guilty-until-proven-safe.

## Ground truth (don't contradict these)

- **Zygisk API version is 5.** `jni/zygisk.hpp` is **not committed** — `build.sh`
  fetches it from a **pinned commit** and **SHA256-verifies** it
  (`ZYGISK_HPP_COMMIT` / `ZYGISK_HPP_SHA256`, `build.sh:29-49`). Don't assume the
  header is in-tree, and don't write code against a different API version without
  updating that pin.
- **Hook inventory** (know what exists before flagging a "missing" hook):
  - JNI native-method hooks via `api->hookJniNativeMethods` on
    `android/os/SystemProperties`: `native_get`, `native_get_int`,
    `native_get_long`, `native_get_boolean` (`main.cpp:193-198,306-317`) — these
    use the **standard** `(JNIEnv*, jclass, …)` convention.
  - **PLT hooks** via `api->pltHookRegister` / `pltHookCommit` on
    `clock_gettime` in `libutils.so` + `libandroid_runtime.so`
    (`main.cpp:374-385`).
  - Direct static-field spoofing on `Build` / `Build$VERSION`
    (`main.cpp:535-567`); `CODENAME` is intentionally forced to `"REL"`.
  - **LSPlant + Dobby** inline hook on `Settings$Secure.getString` — **opt-in,
    OFF by default** (`SBX_ENABLE_LSPLANT`, `CMakeLists.txt:35`;
    `#ifdef` guarded); its generated `hook_dex.h` is gitignored. Treat per-app
    `ANDROID_ID` as inactive in released builds.

## Correctness rules

- **Clock virtualization offsets `CLOCK_BOOTTIME` (and `CLOCK_BOOTTIME_ALARM`)
  ONLY** (`main.cpp:338-343`). `CLOCK_MONOTONIC` must stay **real** — it backs
  internal timing (companion death watcher `companion.cpp:228,233`, crash
  watchdog `main.cpp:418,455`). Offsetting `CLOCK_MONOTONIC` reintroduces the
  timed-wait/hang bug and is **Critical**. The hook must remain a **no-op when
  `UPTIME_SECONDS <= 0`** or empty (`main.cpp:367-372`).
- **Uptime spoofing lives in the PLT `clock_gettime` hook, not a `SystemClock`
  JNI hook.** An earlier `SystemClock` `@CriticalNative` JNI hook was
  *removed* because `@CriticalNative` methods are called by ART with **no
  `JNIEnv*`/`jclass`** (zero-arg) — a normal-convention signature there → ABI
  mismatch → SIGSEGV. Do **not** reintroduce a `SystemClock` JNI hook; if anyone
  does, its signature must match ART's `@CriticalNative` calling convention
  exactly. There are currently **no `@CriticalNative` registrations** in-tree.
- **JNI method signatures must match the descriptor exactly** for every
  `hookJniNativeMethods` entry — a wrong `(Ljava/lang/String;…)` descriptor
  silently fails to bind or crashes. Verify against the AOSP method being hooked.
- **`exemptFd` can return false on the USAP/specialize path of strict Zygisk
  providers** (KernelSU/NeoZygisk etc.) — Magisk short-circuits it, so a bug here
  is invisible to Magisk users. Any fd or mount that depends on `exemptFd`
  succeeding must **degrade gracefully**, not assume success.
- **Mounts happen in a forked child via `setns`**, never directly in the zygote
  process — see the mount/namespace invariants in `10-security.md`.

## Boot-stage safety

- `apply-boot` (`service.sh`) and `seed` (`post-fs-data.sh`) run during boot. A
  hang, deadlock, or crash here bootloops the device. Both are gated on a
  **non-empty `target.txt`** (`service.sh:8`, `post-fs-data.sh:13`) — do not
  remove that guard, and keep boot-stage work bounded and fail-open.

## Build / compiler constraints (`jni/CMakeLists.txt`)

- C++20; release flags `-Os -Wall -Wextra -flto -fvisibility=hidden
  -fno-exceptions -fno-rtti` (`:19`). **The main module is `-fno-exceptions
  -fno-rtti`** — do not add `throw`/`try` or RTTI (`typeid`, `dynamic_cast`) to
  it; only the opt-in LSPlant/Dobby subtargets enable exceptions/RTTI (`:46-50`).
- **There is NO `-Werror`.** CI compiles with `-Wall -Wextra` but warnings are
  **not fatal**, so a real `-Wall`/`-Wextra` warning will merge silently — treat
  a new compiler warning as a finding *yourself*, don't assume CI blocks it.
- `build.sh` always passes `-DCMAKE_BUILD_TYPE=Release`; the debug variant is
  `-DSBX_DEBUG=ON` (rewrites the `*_RELEASE` flag vars). Both variants build all
  four ABIs (`arm64-v8a armeabi-v7a x86_64 x86`).

## Native producers of parsed strings

Several parse-token producers are **native**, not shell — see
`40-webui-and-parse-contract.md` / the safelist. Notably `sandboxid freshen`
prints `OK - fresh persona ready` and the `  MODEL       : %s` row, and the
`identity.prop` serialize order defines the key vocabulary. Editing these C++
strings breaks the WebUI just as a shell edit would.
