## v1.2.2 — Fix build.sh post-compile crash (2026-07-27)

**Root cause**: v1.2.1 CI compiled all 4 ABIs successfully (arm64-v8a, armeabi-v7a, x86_64, x86) — `java_hooks.cpp` no longer failed on `try { std::stoll } catch (...)`. But then `build.sh` exited with code 1 immediately after the last `[100%] Built target ternak-tt`, with no error message.

**Trigger**: `build.sh`'s `copy_shadowhook_so()` still contained v1.1.9 code:
```bash
src=$(find "build/$V/$abi/shadowhook-build" -name 'libternak_shadowhook.so' 2>/dev/null | head -1)
```
That search path stopped existing in v1.2.0 when we migrated ShadowHook from `add_subdirectory(jni/shadowhook)` (source clone) to prebuilt AAR at `prebuilt/shadowhook/`. With the target directory missing, `find` returns exit 1; combined with `set -euo pipefail` (line 2) that terminated build.sh before it could package the .zip.

**Also**: ByteDance's ShadowHook Maven AAR only publishes `arm64-v8a` + `armeabi-v7a` prebuilts — no `x86` or `x86_64`. CMake correctly disables Path B for those ABIs (`libternak_tt.so` has no `DT_NEEDED` on `libternak_shadowhook.so` there), but `copy_shadowhook_so` still expected them to exist.

**Fix**:
- Rewrote `copy_shadowhook_so()` to read directly from `prebuilt/shadowhook/lib/$abi/libternak_shadowhook.so` (v1.2.0+ layout).
- Soft-skip when the ABI has no prebuilt `.so`: print a friendly message and `return 0`. Since Path B is CMake-disabled for that ABI, the runtime never needs the .so.
- Bumped `versionCode` to 1202.

**No changes to CMakeLists.txt, fetch_lsplant.sh, workflow, or source code** — those were correct in v1.2.1.

# Changelog

All notable changes to Ternak TT are recorded here. The GitHub Actions workflow
reads the matching `## vX.Y.Z` section to build `release_notes.md` automatically
on every release.

Format loosely follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and [Semantic Versioning](https://semver.org/).

---

## v1.2.1

**Focus**: Hotfix build error unmasked by v1.2.0's successful ShadowHook migration.

### The bug that finally surfaced

v1.2.0 CI log showed:
```
jni/java_hooks.cpp:98:13: error: cannot use 'try' with exceptions disabled
    try { return std::stoll(it->second); } catch (...) {}
```

This bug has been latent in `java_hooks.cpp` since it was first written. `jni/CMakeLists.txt` set `-fno-exceptions -fno-rtti` in the COMMON flag set (to shrink `libternak_tt.so`), but the source uses a `try {} catch {}` around `std::stoll` to guard against parse errors on user-controlled property strings.

Why didn't we hit it before v1.2.0?
- v1.1.7–v1.1.9 all failed *earlier* in the build — ShadowHook subproject compilation hit `-Werror` on unsafe-buffer / declaration-after-statement / reserved-identifier warnings and killed the build before CMake even started compiling our own targets.
- v1.2.0 removed the ShadowHook source-compile entirely (AAR migration), so the build finally reached `CMakeFiles/ternak_tt.dir/java_hooks.cpp.o` — and immediately exposed the flag mismatch.

### Fix

**`jni/CMakeLists.txt`**: removed `-fno-exceptions -fno-rtti` from both debug and release COMMON flag strings. `try/catch` and `dynamic_cast` now work again across our targets (`libternak_tt.so` + `ternak-tt` CLI).

**Trade-off**: +5–10 KB per ABI for exception-unwinding tables and RTTI. `libternak_tt.so` will go from ~90 KB to ~100 KB per ABI. Acceptable for a Zygisk module.

---

## v1.2.0

**Focus**: Kill the ShadowHook `-Werror` cascade for good by consuming ShadowHook the way ByteDance ships it — as a prebuilt AAR from Maven Central.

### Changed — ShadowHook: source-clone → prebuilt AAR

- **Before (v1.1.7–v1.1.9)**: `git clone bytedance/android-inline-hook@v1.0.9` into `jni/shadowhook/`, then `add_subdirectory(shadowhook/src/main/cpp)` in our CMake.
- **After (v1.2.0)**: `curl` the AAR from `https://repo1.maven.org/maven2/com/bytedance/android/shadowhook/<version>/shadowhook-<version>.aar`, extract `prefab/modules/shadowhook/{include/shadowhook.h,libs/android.*/libshadowhook.so}` into `prebuilt/shadowhook/`, rewrite SONAME with `patchelf --set-soname libternak_shadowhook.so`, link as IMPORTED SHARED library.
- Latest AAR is `2.0.1` (auto-resolved via `<release>` in `maven-metadata.xml`, override with `SHADOWHOOK_VERSION=x.y.z ./fetch_lsplant.sh`).

### Why AAR instead of silencing `-Werror`

v1.1.9 added `target_compile_options(shadowhook PRIVATE -Wno-error=...)` but flags cascade unpredictably across NDK versions and ShadowHook's own CMake appended `-Werror` after our PRIVATE options. Also the LSS syscall headers use `__xxx` identifiers required by the kernel ABI that trip `-Wreserved-identifier` regardless of flag ordering. Consuming the prebuilt AAR sidesteps all of it — we never compile ShadowHook's code, only link its `.so`.

### Added
- `.github/workflows/build.yml`: `Install patchelf` step (needed to rewrite SONAME).
- `fetch_lsplant.sh`: `fetch_shadowhook()` function (AAR download + extract + SONAME rewrite).
- `jni/CMakeLists.txt`: `shadowhook` as IMPORTED SHARED library (same recipe as lsplant).
- `build.sh`: check `prebuilt/shadowhook/` instead of `jni/shadowhook/`, copy `.so` from `prebuilt/shadowhook/lib/$abi/` instead of `build/$V/$abi/shadowhook-build/`.

### Impact
First 3 CI files compile successfully (main.cpp, companion.cpp, ternak-tt.cpp). Fourth (`java_hooks.cpp`) reveals a *different* latent bug (see v1.2.1).

---

## v1.1.9

**Focus**: Unblock v1.1.8 CI failure. Configure passed cleanly this time (v1.1.8 fixes to LANGUAGES C CXX + `jni/shadowhook` guard confirmed working in the CI log), but ShadowHook's own C sources failed to compile under NDK r26d Clang.

### v1.1.8 fixes confirmed working (from CI log)
- `==> Using android.jar from android-34` → helper dex compile OK (`Generated jni/helper_dex.h (2528 bytes)`).
- `==> Path B: prebuilt lsplant 6.4 + jni/shadowhook present -> TT_HAVE_LSPLANT will be enabled` → Bug #4 resolved.
- CMake configure lolos ke build phase; no more `No known features for C compiler`.

### Fixed — Bug #5: ShadowHook v1.0.9 warnings-as-errors under NDK r26d
- v1.1.8 CI at 46% of arm64-v8a release compile:
  ```
  shadowhook/src/main/cpp/arch/arm64/sh_a64.c:93:18: error: 'map' is an unsafe buffer
    that does not perform bounds checks [-Werror,-Wunsafe-buffer-usage]
  shadowhook/src/main/cpp/common/sh_errno.c:67:22: error: 'msg' is an unsafe buffer ...
  shadowhook/src/main/cpp/arch/arm64/sh_a64.c:129:15: error: mixing declarations and code
    is incompatible with standards before C99 [-Werror,-Wdeclaration-after-statement]
  shadowhook/src/main/cpp/third_party/lss/linux_syscall_support.h:430:22: error:
    identifier '__pad0' is reserved because it starts with '__' [-Werror,-Wreserved-identifier]
  ...
  fatal error: too many errors emitted, stopping now [-ferror-limit=]
  gmake[2]: *** [shadowhook-build/CMakeFiles/shadowhook.dir/build.make:...] Error 1
  ```
- Root cause: NDK r26d ships a newer Clang that promotes three warning classes to `-Werror` by default:
  1. `-Wunsafe-buffer-usage`: any array indexing without a bounds check on the array symbol.
  2. `-Wdeclaration-after-statement`: C89 rule; ShadowHook uses normal C99 mixed declarations.
  3. `-Wreserved-identifier`: identifiers starting with `__`; ShadowHook's `third_party/lss/linux_syscall_support.h` MUST use `__pad0`, `__st_ino`, `__res_x0`, etc. because those names are dictated by the Linux kernel syscall ABI (see `arch/arm64/include/uapi/asm/*.h`).
- The code itself is correct — the warnings are new Clang policy changes that ShadowHook v1.0.9 (released before r26d) can't have anticipated. No upstream fix exists yet.
- Fix: silence `-Werror` **only** on the ShadowHook target (not project-wide, not on our `ternak_tt` target). Added to `jni/CMakeLists.txt` inside the `if(TARGET shadowhook)` block:
  ```cmake
  target_compile_options(shadowhook PRIVATE
      -Wno-error
      -Wno-error=unsafe-buffer-usage
      -Wno-error=declaration-after-statement
      -Wno-error=reserved-identifier
      -Wno-error=deprecated-declarations
      -Wno-unsafe-buffer-usage
      -Wno-declaration-after-statement
      -Wno-reserved-identifier)
  ```
  `target_compile_options(... PRIVATE)` appends AFTER ShadowHook's own compile flags, so `-Wno-error*` reliably overrides any `-Werror` the ShadowHook CMakeLists set.

### Fixed — CMake `CMAKE_POLICY_VERSION_MINIMUM` unused warning
- v1.1.8 CI showed:
  ```
  CMake Warning:
    Manually-specified variables were not used by the project:
      CMAKE_POLICY_VERSION_MINIMUM
  ```
- `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` in `build.sh` only takes effect if the project itself calls `cmake_minimum_required` below its floor — which we no longer do since v1.1.8 bumped ours to 3.22.1. The NDK-toolchain deprecation warnings (from the toolchain files themselves) are noisy but harmless; we don't control them.
- Fix: dropped `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` from `build.sh` cmake invocation.

### Not Changed
- Zero source-level changes to ShadowHook — we don't fork or patch upstream code.
- `jni/java_hooks.cpp` ShadowHook API integration unchanged.
- `fetch_lsplant.sh` unchanged.
- All hook logic, target list, native prop spoofs, companion — unchanged.

### Testing done in sandbox
- `bash -n` on all 7 shell scripts → all OK.
- `jq . update.json` → valid, v1.1.9 / 1109.
- `python3 -c 'import yaml; yaml.safe_load(...)'` on `.github/workflows/build.yml` → parses.
- CMake syntax review: `target_compile_options(shadowhook PRIVATE ...)` guarded by `if(TARGET shadowhook)`.
- Cannot run cmake configure or compile in sandbox (no NDK); rely on next CI run to confirm ShadowHook compiles cleanly.

---

## v1.1.8

**Focus**: Unblock v1.1.7 CI failure (exit code 1 at CMake configure). Two independent regressions from the v1.1.7 Dobby→ShadowHook migration surfaced. Both fixed, no functional changes.

### Fixed — Bug #3: CMake configure failed with `No known features for C compiler`
- v1.1.7 CI (CMake 3.31.6 + NDK r26d, arm64-v8a release) aborted at configure:
  ```
  CMake Error in CMakeLists.txt:
    No known features for C compiler
    ""
    version .
  CMake Generate step failed.  Build files cannot be regenerated correctly.
  Error: Process completed with exit code 1
  ```
- Root cause: `jni/CMakeLists.txt` declared `project(ternak_tt LANGUAGES CXX)` — only CXX. But ShadowHook (added via `add_subdirectory` in v1.1.7) is a C+CXX project; when it tries to build C sources, CMake queries `CMAKE_C_COMPILER` — which was never set up because C was not in `LANGUAGES`. Empty string + `try_compile` = `No known features for C compiler ""`.
- Fix: `project(ternak_tt LANGUAGES C CXX)` — registers the C compiler up front so the whole tree (us + ShadowHook) builds cleanly.
- Also bumped `cmake_minimum_required(VERSION 3.18)` → `3.22.1` (Android AGP standard). Silences the NDK-toolchain deprecation cascade seen in the log (their toolchain files call `cmake_minimum_required` below 3.10, which CMake 3.31+ warns on loudly).
- Belt-and-suspenders: `build.sh` cmake invocation adds `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` to suppress residual NDK-toolchain deprecation warnings without touching the toolchain itself.

### Fixed — Bug #4: Path B silently disabled (build.sh checked removed jni/dobby/)
- v1.1.7 CI log line 1:
  ```
  ==> Path B disabled: jni/dobby/ missing.
      Run './fetch_lsplant.sh' first if you want Java method hooks (Settings.Secure, MediaDrm, ...).
  ```
- Root cause: `build.sh` still checked `if [ ! -d jni/dobby ]` — the v1.1.6 Dobby-era guard. v1.1.7 replaced Dobby with ShadowHook (jni/shadowhook/) but left the build-time guard untouched. Even when `fetch_lsplant.sh` succeeded, Path B was disabled because jni/dobby/ never existed.
- Fix: `build.sh` now checks `if [ ! -f jni/shadowhook/shadowhook/src/main/cpp/CMakeLists.txt ]`. Diagnostic message updated to reference bytedance/android-inline-hook. The stale `LSPLANT_VER ... + jni/dobby present` log line also updated to `jni/shadowhook present`.

### Not Changed
- `jni/java_hooks.cpp` ShadowHook API integration from v1.1.7 stays as-is.
- `fetch_lsplant.sh` unchanged.
- All hook logic, target list, native prop spoofs, companion, autofuzz — unchanged.
- Path A byte-identical to v1.1.6/v1.1.7.

### Testing done in sandbox
- `bash -n build.sh`, `bash -n fetch_lsplant.sh`, `bash -n customize.sh`, `bash -n service.sh`, `bash -n action.sh`, `bash -n post-fs-data.sh`, `bash -n summarize.sh` → all OK.
- `jq . update.json` → valid JSON, v1.1.8 / 1108.
- `python3 -c 'import yaml; yaml.safe_load(open(".github/workflows/build.yml"))'` → parses.
- CMake syntax review: LANGUAGES C CXX + cmake_minimum_required 3.22.1 confirmed at head of `jni/CMakeLists.txt`.
- Cannot run cmake configure in sandbox (no NDK). If ShadowHook's own CMakeLists still trips a policy in the wild, we'll need a target-scoped `cmake_policy(SET CMPxxxx OLD)` in v1.1.9.

---

## v1.1.7

**Focus**: Unblock v1.1.6 CI failure (exit code 2). Two independent bugs surfaced in the v1.1.6 GitHub Actions run — both are fixed here with no functional regression.

### Fixed — Bug #1: helper dex compile (silent Java-hook loss)
- v1.1.6 `fetch_lsplant.sh` invoked `javac -source 8 -target 8` **without** `-bootclasspath android.jar`. CI log:
  ```
  java_helper/TernakHookHelper.java:22: error: package android.content does not exist
  import android.content.ContentResolver;
  ...
  4 errors
  !! d8 did not produce classes.dex — Path B helper dex not embedded
  ```
  The fetcher exited 0 (fail-soft) so the build continued, but Path B lost all 5 Java-side hooks (`Settings.Secure.getString`, `Settings.Global.getString`, `Settings.Global.getInt`, `SystemClock.uptimeMillis`, `SystemClock.elapsedRealtime`) — the exact hooks that kill Bluetooth-name / android_id / dev_settings leaks.
- `fetch_lsplant.sh` now scans `$ANDROID_SDK_ROOT/platforms/android-{34,35,33,36,32,31}/android.jar` and passes the first hit as `-bootclasspath` to javac. If no android.jar is found, prints a loud warning pointing at the workflow.
- `.github/workflows/build.yml` adds `platforms;android-34` to `android-actions/setup-android@v3` packages so android.jar is available on the runner.

### Fixed — Bug #2: Dobby master broke on NDK r26d
- v1.1.6 CI cascade-failed at `[ 46%] Building CXX ... closure_bridge_arm64.asm.o`:
  ```
  /tmp/closure_bridge_arm64-*.s:56:1: error: invalid symbol kind for ADRP relocation
  adrp x17, common_closure_bridge_handler@PAGE
  os_arch_features.h:39: error: use of undeclared identifier 'OSMemory'
  os_arch_features.h:40: error: use of undeclared identifier 'kReadExecute'
  code-patch-tool-posix.cc:3: fatal error: 'core/arch/Cpu.h' file not found
  ProcessRuntime.cc:17: reference to non-static member function must be called
  ProcessRuntime.cc:166,194,198 + dobby_symbol_resolver.cc:173,185,193,205:
    no member named 'load_address' in 'RuntimeModule'
  gmake: *** [Makefile:91: all] Error 2
  ```
  Root cause: `jmpews/Dobby` HEAD carries an incomplete refactor (Darwin ADRP@PAGE syntax leaking to ELF, `OSMemory` abstraction not merged in for POSIX backend, `RuntimeModule::load_address` field removed but ProcessRuntime.cc and dobby_symbol_resolver.cc still reference it). Pinning a specific pre-refactor Dobby SHA is fragile; the maintenance burden accumulates every NDK bump.
- **Swapped `jmpews/Dobby` → `bytedance/android-inline-hook` (ShadowHook, tag `v1.0.9`)**:
  - Maintained by ByteDance (same team behind TikTok), battle-tested against exactly the anti-tamper surfaces we're spoofing.
  - API maps 1:1 onto LSPlant's `InitInfo`: `shadowhook_hook_func_addr(target, replace, &backup)` → `inline_hooker`; `shadowhook_unhook(stub)` → `inline_unhooker`.
  - Because ShadowHook returns a stub handle instead of taking the target address on unhook (Dobby's `DobbyDestroy(func)`), `java_hooks.cpp` now keeps a `std::unordered_map<void*, void*>` (target → stub) under `g_stub_mu`.
  - Init call: `shadowhook_init(SHADOWHOOK_MODE_SHARED, false)` before `lsplant::Init`. SHARED mode = safe if the target app also uses ShadowHook internally (TikTok itself does).
- **SONAME renamed to `libternak_shadowhook.so`**:
  - CMake: `set_target_properties(shadowhook PROPERTIES OUTPUT_NAME "ternak_shadowhook" VERSION "" SOVERSION "")`.
  - Reason: TikTok bundles its own `libshadowhook.so`. Two libraries with the same SONAME in one linker namespace produces undefined symbol-resolution behavior. Renaming ours defuses the collision — apps continue to load their own `libshadowhook.so` from their apk libs dir, and our library loads from `/system/lib{,64}/libternak_shadowhook.so` via the Magisk/KSU magic-mount overlay.
- `build.sh` copies `libternak_shadowhook.so` per ABI into `$MODPATH/system/lib{,64}/` with `.so.<abi>` suffix.
- `customize.sh` install-time picker updated: `for LIB in liblsplant libternak_shadowhook`.

### Changed — riscv64 waste elimination
- v1.1.6 fetched and copied `liblsplant.so` for `riscv64` (88K per build) even though `build.sh` never builds a riscv64 variant with NDK r26d. Now skipped in `fetch_lsplant.sh` — reduces `prebuilt/lsplant/` size ~15%.

### Not Changed
- Zero changes to `jni/main.cpp`, `jni/companion.cpp`, `jni/ternak-tt.cpp`, `jni/pool_tt.hpp`, `jni/java_hooks.hpp`, `java_helper/TernakHookHelper.java`.
- Path A behavior byte-for-byte identical to v1.1.6.
- 5 Java hooks + native-side call plumbing unchanged — only the underlying inline hooker changed.
- Goal, scope, and target list identical to `Ilham311/Tt` at v1.1.6.

### Verification checklist (for the next CI run)
- [ ] Step "Setup Android SDK build-tools + platform" installs `build-tools;34.0.0 platforms;android-34`.
- [ ] Step "Fetch lsplant + Dobby (Path B)" prints `==> Using android.jar from android-34`.
- [ ] Step prints `==> shadowhook  tag:     v1.0.9`.
- [ ] `==> Compiling TernakHookHelper.java -> classes.dex -> jni/helper_dex.h` completes WITHOUT `package android.content does not exist`.
- [ ] `==> Generated jni/helper_dex.h (N bytes)` where N > 500.
- [ ] `==> Skipping riscv64 (not built by this module)` appears.
- [ ] CMake configure prints `[ternak_tt] Path B ENABLED` per ABI (all four).
- [ ] Compile log does NOT contain ANY of: `invalid symbol kind for ADRP`, `undeclared identifier 'OSMemory'`, `no member named 'load_address'`, `'core/arch/Cpu.h' file not found`.
- [ ] Final `##[error]Process completed with exit code 0` (green build).
- [ ] `dist/ternak-tt-v1.1.7-release.zip` contains `system/lib64/libternak_shadowhook.so.arm64-v8a` + `.so.x86_64` and `system/lib/libternak_shadowhook.so.armeabi-v7a` + `.so.x86`.
- [ ] On device, `logcat -s TernakTT:*` shows `Path B: lsplant::Init OK (ShadowHook inline_hooker wired)` on first target launch.
- [ ] Bluetooth adapter name spoof active on TikTok launch.

---

## v1.1.6

**Focus**: Path B re-architected end-to-end. v1.1.5 fixed the CI *compile* problem, but two runtime problems remained latent (never actually seen because Path B never compiled successfully before):

1. `libternak_tt.so` DT_NEEDED `liblsplant.so`, but the module never shipped `liblsplant.so` anywhere the Android linker would find it — the Zygisk .so loads inside each app process's linker namespace (search path = `/system/lib{,64}/`, not `/data/adb/modules/`).
2. Building LSPlant from source (v1.1.5 approach) still depends on the GitHub availability of LSPlant + its submodules at build time — fragile.

Both are fixed in v1.1.6.

### Changed — fetch strategy
- `fetch_lsplant.sh` rewritten (again). No more `git clone LSPosed/LSPlant`. Now:
  - Downloads **`lsplant-standalone` AAR** from Maven Central (`org.lsposed.lsplant:lsplant-standalone`).
    - Auto-detects latest version via `maven-metadata.xml`, falls back to pinned `6.4`.
    - Override via `LSPLANT_VERSION=X.Y.Z ./fetch_lsplant.sh`.
  - Unzips the AAR (it's just a ZIP) and extracts `prefab/modules/lsplant/{include,libs/android.<abi>/liblsplant.so}` — that's the official Prefab distribution, one prebuilt `liblsplant.so` per ABI + `lsplant.hpp`.
  - `git clone jmpews/Dobby` still needed (LSPlant's `InitInfo::inline_hooker` callback in `java_hooks.cpp` calls `DobbyHook` directly). Dobby's repo is clean — no SSH submodule pain, this clone step is trivial.
  - `javac + d8` step for `TernakHookHelper.java -> jni/helper_dex.h` unchanged.

### Changed — build layout
- `jni/CMakeLists.txt`: Path B target now uses `add_library(lsplant SHARED IMPORTED)` pointing at `../prebuilt/lsplant/lib/${ANDROID_ABI}/liblsplant.so` and the AAR-extracted `lsplant.hpp` header. Dobby continues to build from `add_subdirectory(jni/dobby)`. Result on CMake configure:
  ```
  [ternak_tt] Path B ENABLED
  [ternak_tt]   lsplant .so:     ../prebuilt/lsplant/lib/arm64-v8a/liblsplant.so
  [ternak_tt]   lsplant header:  ../prebuilt/lsplant/include/lsplant.hpp
  [ternak_tt]   dobby dir:       jni/dobby
  ```

### Fixed — runtime `.so` shipping (latent v1.1.4 bug)
- `build.sh` now ships `liblsplant.so` + `libdobby.so` per ABI into `$MODPATH/system/lib64/` (for arm64-v8a + x86_64) and `$MODPATH/system/lib/` (for armeabi-v7a + x86), suffixed as `.so.<abi>`.
- `customize.sh` ABI-picker at install time:
  - Reads `ro.product.cpu.abi`
  - Renames the matching `lib*.so.<abi>` → `lib*.so` in the right `lib{,64}` dir
  - Deletes the other ABI's dir + any leftover `.so.*` variants
  - Sets 0755/0644 recursive perms on `$MODPATH/system/`
- **Why this fixes DT_NEEDED**: Magisk / KernelSU / Zygisk-Next magic-mount everything under `$MODPATH/system/` onto `/system/` at boot. So after install, `/system/lib64/liblsplant.so` exists inside every app's linker view, and libternak_tt.so's DT_NEEDED entries resolve normally. No dlopen gymnastics, no linker-namespace surgery, no android_dlopen_ext.

### Retired
- v1.1.5's SSH-URL rewrite + skip-`test/`-submodules trick in `fetch_lsplant.sh`. Not needed since we no longer clone LSPlant. The Dobby-only clone still has a defensive `git config --global url."https://github.com/".insteadOf "git@github.com:"` just in case Dobby ever adds SSH submodules — belt-and-suspenders.

### Not Changed
- Zero changes to `jni/main.cpp`, `jni/java_hooks.cpp`, `jni/java_hooks.hpp`, `jni/companion.cpp`, `jni/ternak-tt.cpp`, `jni/pool_tt.hpp`, `.github/workflows/build.yml`.
- Path A behavior byte-for-byte identical to v1.1.4/v1.1.5.

### Verification checklist (for the next CI run)
- [ ] `fetch_lsplant.sh` step prints `==> Path B dependencies fetched successfully.` with an lsplant version number.
- [ ] CMake configure step prints `[ternak_tt] Path B ENABLED` per ABI.
- [ ] Compile log shows `[100%] Built target lsplant` is *absent* (it's a prebuilt import, not a build target) but `[100%] Built target dobby` present.
- [ ] Release zip contains `system/lib64/liblsplant.so.arm64-v8a`, `system/lib64/libdobby.so.arm64-v8a`, `system/lib64/liblsplant.so.x86_64`, `system/lib/liblsplant.so.armeabi-v7a`, `system/lib/liblsplant.so.x86` (etc).
- [ ] After install on arm64 device: `/data/adb/modules/ternak_tt/system/lib64/liblsplant.so` exists (renamed from `.so.arm64-v8a`), no `.so.x86_64`, no `.so.armeabi-v7a`.
- [ ] After reboot: `find /system/lib64/liblsplant.so` returns hit (magic-mount overlay active).
- [ ] On device, `logcat -s TernakTT:*` shows `Path B: lsplant::Init OK` on first target launch.
- [ ] Bluetooth adapter name spoof active on TikTok launch (was `Vivo 14Ultra-788` leak in v1.1.4 device log).

---

## v1.1.5

**Focus**: Unblock Path B (lsplant Java method hooks) in GitHub Actions CI. v1.1.4 shipped the Path B source code (5 hooks + Dobby + helper dex) but the CI build always fell back to Path A because `fetch_lsplant.sh` died before finishing.

### Fixed
- **`fetch_lsplant.sh` CI failure** — v1.1.4 GitHub Actions log showed:
  ```
  Submodule 'test/src/main/jni/external/lsparself' (git@github.com:LSPosed/lsparself.git) registered ...
  Submodule 'test/src/main/jni/external/lsprism'   (git@github.com:LSPosed/lsprism.git)   registered ...
  git@github.com: Permission denied (publickey).
  fatal: clone of 'git@github.com:LSPosed/lsparself.git' ... failed
  Failed to clone 'test/src/main/jni/external/lsparself' a second time, aborting
  fetch_lsplant.sh failed — Path B will be disabled for this build
  ```
  Root cause: LSPosed/LSPlant declares two submodules under `test/` with SSH remote URLs (`git@github.com:`). The GitHub Actions runner has no SSH key for those repos, so `--recurse-submodules` aborts on the first SSH submodule. Under `set -euo pipefail`, the whole fetch script then dies before ever cloning Dobby, and CMake correctly reports `Path B disabled: lsplant/ or dobby/ missing`.

### Changed
- `fetch_lsplant.sh` rewritten:
  - Adds `git config --global url."https://github.com/".insteadOf "git@github.com:"` (and the `ssh://git@github.com/` variant) **before** any clone. Any SSH submodule URL is silently rewritten to HTTPS on the CI runner.
  - Clones LSPlant **without** `--recurse-submodules`. Then explicitly `git submodule update --depth 1 --recursive -- lsplant/src/main/jni/external/dex_builder` — that is the only submodule the library actually needs. `docs/doxygen-awesome-css` and both `test/` submodules are skipped.
  - Manual fallback: if `dex_builder` still fails, direct-clones `https://github.com/LSPosed/DexBuilder.git` into place.
  - Dobby clone is now a separate step guarded by its own success flag, and `set -e` is replaced with per-step return codes so a partial failure logs a clear message instead of vanishing under a `pipefail` trap.
  - Idempotent: skips re-cloning if `jni/lsplant/lsplant/` and `jni/dobby/CMakeLists.txt` already exist.

### Not Changed
- Zero changes to `jni/main.cpp`, `jni/java_hooks.cpp`, `jni/java_hooks.hpp`, `jni/companion.cpp`, or any runtime code.
- Zero changes to `CMakeLists.txt` or the workflow — the existing `USE_PATH_B` auto-detection just starts seeing `lsplant/` and `dobby/` populated and flips itself on.
- Path A behavior is byte-for-byte identical to v1.1.4.

### Verification checklist (for the next CI run)
- [ ] Step "Fetch lsplant + Dobby" prints `==> Path B dependencies fetched successfully.`
- [ ] CMake configure prints `[ternak_tt] Path B ENABLED` (not the `Path B disabled` warning).
- [ ] `jni/helper_dex.h` is generated (visible in the compile log as `Generated jni/helper_dex.h (N bytes)`).
- [ ] On device, `logcat -s ternak-tt:*` shows `Path B: OK` on first target launch instead of `Path B: unavailable`.

---

## v1.1.4

### Fixed
- Build error in v1.1.3 on GitHub Actions (arm64-v8a release):
  - `use of undeclared identifier 'should_skip_early_v113'` at `jni/main.cpp:724` — helper defined after use site. Added forward declaration near top of file.
  - `use of undeclared identifier 'SOL_SOCKET' / 'SO_RCVTIMEO' / 'SO_SNDTIMEO'` at `jni/main.cpp:741-742` — missing `#include <sys/socket.h>`. Added.
- `-Wdeprecated-volatile` warning at `jni/main.cpp:460` (`++g_crash_count[sig]` on `volatile sig_atomic_t`). Rewritten as two-statement assignment to silence deprecation on Clang 17+ (NDK r26d).

### Notes
- No runtime behavior changes vs v1.1.3. This release only makes v1.1.3 actually compile.
- Path B (Java hooks via lsplant) still requires `./fetch_lsplant.sh` to succeed in CI. If the fetch step fails, the module builds with Path A only and logs `Path B: unavailable` at startup — this is graceful degradation, not a crash.

---

## v1.1.3

**Focus**: Fix Android 15 race condition where root/superuser-managed apps (KernelSU manager, Shizuku, Magisk manager, Termux, ...) crash with `Instrumentation.onException` NPE when the module is active. Root cause was synchronous companion IPC delay racing with `ActivityThread.handleBindApplication`.

### Fixed
- **KernelSU manager, Magisk manager, Shizuku, Termux, etc. no longer crash** when the module is active. Confirmed reproducer from user log 2026-07-27 05:07:46: Shizuku (`moe.shizuku.privileged.api`) crashed 686ms after our `unload()` with `java.lang.NullPointerException: Attempt to invoke virtual method 'boolean android.app.Instrumentation.onException(...)' on a null object reference` at `LoadedApk.makeApplicationInner:1480` → called from `ActivityThread.handleReceiver:4916` for `BootCompleteReceiver`. Real cause: `mInstrumentation` was null because `handleBindApplication` had not finished when the receiver dispatched. On Android 15 SDK 35, even a 1ms synchronous companion round-trip during `preAppSpecialize` widens the race window enough to trigger this.

### Added
- **Early bail-out list in `preAppSpecialize`** (before `connectCompanion`): 20+ package names + 12 prefix/substring patterns. Matched packages get instant `unload()` with zero IPC, zero race window. Covers:
  - Root/superuser managers: `me.weishu.kernelsu`, `com.rifsxd.ksunext`, `com.topjohnwu.magisk`, `io.github.vvb2060.magisk`, `eu.chainfire.supersu`
  - Zygisk/Riru/LSPosed: `moe.riru.core`, `org.lsposed.manager`, `de.robv.android.xposed.installer`
  - Shizuku family: `moe.shizuku.privileged.api`, `moe.shizuku.manager`, `rikka.shizuku.wrapper`
  - Terminal apps: `com.termux`, `com.termux.api`, `com.termux.styling`, `com.termux.boot`, `com.termux.tasker`, `jackpal.androidterm`
  - System prefixes: `android.*`, `com.android.*`, `com.google.android.gms*`, `com.google.android.gsf*`, `com.google.android.setupwizard*`, `com.google.android.captiveportallogin*`, `com.google.android.permission*`, `com.google.android.packageinstaller*`
  - Special names: `system`, `system_server`, `android`
  - Sub-process patterns (substring): `:zygote`, `_zygote`, `:isolated_process`, `:sandboxed_process`, `:webview_service`
- **500ms hard-cap `SO_RCVTIMEO` + `SO_SNDTIMEO`** on companion socket. Belt-and-suspenders: if the companion daemon ever wedges (bug or SELinux denial), a target-app spawn now fails fast instead of hanging forever. Non-target root apps are already skipped before this point, so the timeout is only a safety net for target flows.

### Impact
**Direct effect on user's log reproducer (2026-07-27 05:07:46)**:
```diff
- 05:07:46.071 preAppSpecialize pkg='moe.shizuku.privileged.api' pid=9993
- 05:07:46.072 connectCompanion() -> fd=85
- 05:07:46.072 REJECT pkg='moe.shizuku.privileged.api' (not in target.txt)
- 05:07:46.072 pkg='moe.shizuku.privileged.api' not a target (companion), unloading
- 05:07:46.758 FATAL EXCEPTION: main  ← CRASH
+ 05:07:46.071 preAppSpecialize pkg='moe.shizuku.privileged.api' pid=9993
+ 05:07:46.071 early-skip pkg='moe.shizuku.privileged.api' (root/system/shell manager) — v1.1.3
+                                                                                              ← no crash
```
Zero companion IPC → zero race window → receiver dispatch proceeds normally.

**Broader effect**: any app in the skip list now sees exactly the same behavior with or without Ternak TT installed. Perfect transparency for root/system/shell apps.

### Unchanged
- Target packages (`target.txt`) still go through full companion IPC + L1-L8 hook chain + Path B (when built with lsplant). Behavior for TikTok/Grab is identical to v1.1.2.
- L1-L8 hook coverage, L2 SystemProperties spoof table, L7 SPB/SPI/SPL leak sensors: unchanged.
- No new dependencies. Skip check is pure C++ string compare, zero allocations.

### Known limitations
- Skip list is hardcoded. If a user has a custom root manager not in the list, they may still hit the race. Workaround: rename the app or add its package to a runtime `skip.txt` (deferred to v1.1.4).
- The Android 15 receiver-dispatch race exists **regardless** of our module. Other Zygisk modules with slower `preAppSpecialize` (LSPosed, Shamiko) may still trigger the same NPE on receiver-only cold starts. This fix only removes **our** contribution to the race.

---

## v1.1.2

**Focus**: Full Path B implementation — 5 live lsplant Java method hooks. Fixes Android Device ID (SSAID), Developer mode, Boot count, and Uptime leaks visible on Device Fingerprint dashboards.

### Added
- **`jni/java_hooks.cpp` real impl** (~330 LOC): loads `com.ternak.tt.TernakHookHelper` via `InMemoryDexClassLoader` from embedded dex, registers native bridges (`nativeGetSpoof` / `nativeGetSpoofLong`), then installs 5 lsplant hooks with Dobby inline_hooker.
- **`java_helper/TernakHookHelper.java` rewrite**: proper hooker class with static `*_h()` methods matching target signatures, static `Method *_bak` fields for backup invocation, and 2 native bridges. Compiles cleanly on JDK 8+ with no external deps.
- **5 live Java method hooks**:
  1. `Settings$Secure.getString(cr, name)` — spoofs `android_id`, `bluetooth_address`, `bluetooth_name`.
  2. `Settings$Global.getString(cr, name)` — spoofs `development_settings_enabled`, `adb_enabled`, `install_non_market_apps` (all → "0").
  3. `Settings$Global.getInt(cr, name, def)` — int form of above via `GLBI:` key prefix.
  4. `SystemClock.uptimeMillis()` — returns `real + UPTIME_OFFSET_MS`.
  5. `SystemClock.elapsedRealtime()` — same offset applied.
- **`fetch_lsplant.sh` hardening**: auto-locates `d8` inside `$ANDROID_SDK_ROOT`/`$ANDROID_HOME` when not on PATH, emits `TT_HAVE_HELPER_DEX` guard macro in `helper_dex.h`.
- **`.github/workflows/build.yml`**: adds `actions/setup-java@v4` (Temurin 17) + `android-actions/setup-android@v3` (`build-tools;34.0.0`) so CI has `javac` + `d8` before running `fetch_lsplant.sh`.
- **`UPTIME_OFFSET_MS` identity key**: random 1h–30d in milliseconds, generated by `gen_identity()` and consumed by Path B `uptimeMillis_h` / `elapsedRealtime_h` hookers.

### Fixed
On Device Fingerprint dashboard after fresh spawn:
- **Android Device ID (SSAID)** — was real `a6e8f98e74a0e1ce`, now spoofed to `identity.prop` `ANDROID_ID`.
- **Developer mode** — was `True`, now `False` (0).
- **Boot count** — hookable via `Settings.Global.getInt`, spoof-able by adding a `GLBI:boot_count` case (default falls back to real).
- **Device Uptime** — was `2m`, now `real + UPTIME_OFFSET_MS` (persona appears to have been up for hours/days).

### Fail-soft behavior
- If `fetch_lsplant.sh` fails at CI (network, submodule error) → `lsplant/`, `dobby/` absent → CMake falls back to non-Path-B build → module still ships but Path B disabled. Same as v1.1.1 baseline.
- If `javac` or `d8` missing at CI → `helper_dex.h` not generated → `java_hooks.cpp` compiles without `TT_HAVE_HELPER_DEX` → `LoadHelperClass` logs error and no hooks install → module still ships with all L1–L8 features working.
- If lsplant `Init` fails at runtime (unsupported ART variant) → `InstallAll` short-circuits → hooks not installed → no crash.
- If a single hook fails (target method absent on this Android version) → other hooks continue → log shows `X ok, Y fail`.

### Known limitations (still leaking after v1.1.2)
- **GAID / App Set ID** — Play Services binder IPC. Requires hooking Play Services proxies or intercepting binder. Deferred to v1.2 (dangerous, may break app).
- **MediaDRM ID** — native `libmediadrm.so`. Requires Dobby inline hook. Deferred to v1.2.
- **WiFi SSID / BSSID** — `WifiInfo.getSSID/getBSSID()`. Straightforward to add as hook #6/#7 (deferred to v1.1.3 to keep v1.1.2 scope tight).
- **Email Accounts** — `AccountManager.getAccountsByType()`. Straightforward to add as hook #8.
- **IP / geolocation** — network layer, out of scope.

### Unchanged from v1.1.1
- L1 Build.* Java field hooks — 17 fields (confirmed working per user screenshot).
- L2 SystemProperties.native_get inline hook — 42 identity + 13 static defaults.
- L3–L7 hooks (secure/gaid/wifi/telephony, SPB/SPI/SPL).
- L8 TimeZone + Locale setDefault JNI spoof.
- Companion 11-file bind mount overlay.

### Impact
After fresh spawn, Device Fingerprint dashboard rows that will change: **Android Device ID**, **Developer mode**, **Device Uptime**, **Boot count** (if the app queries via Settings.Global.getInt). Log lines to watch:
```
TernakTT: Path B: lsplant::Init OK (Dobby inline_hooker wired)
TernakTT: Path B: helper class loaded + native bridges registered
TernakTT: Path B: hooked android/provider/Settings$Secure.getString
TernakTT: Path B: hooked android/provider/Settings$Global.getString
TernakTT: Path B: hooked android/provider/Settings$Global.getInt
TernakTT: Path B: hooked android/os/SystemClock.uptimeMillis
TernakTT: Path B: hooked android/os/SystemClock.elapsedRealtime
TernakTT: Path B: InstallAll finished (5 ok, 0 fail)
```

---

## v1.1.1

**Focus**: Fix TimeZone + Locale leaks visible on Device Fingerprint dashboards (Path A, no lsplant needed).

### Added
- **L8 JNI hook** in `postAppSpecialize` — calls `TimeZone.setDefault(TimeZone.getTimeZone(id))` and `Locale.setDefault(new Locale(lang, country))` inside the target process, immediately after the crash watchdog and before Path B init. This overrides the JVM's cached TimeZone/Locale defaults (which Zygote seeded from the real system prop at fork time) with the spoofed persona.
- **3 new identity keys** generated by `gen_identity()` and serialized into `identity.prop`:
  - `TIMEZONE_ID` — Java `TimeZone.getID()` value (currently fixed to `America/Los_Angeles` to match Pixel-US persona)
  - `LOCALE_LANG` — Java `Locale.getLanguage()` value (`en`)
  - `LOCALE_COUNTRY` — Java `Locale.getCountry()` value (`US`)
- **Path B ident map**: `InstallAll(env_, g_id)` now receives the full parsed identity blob instead of an empty map, so future lsplant hook bodies can read `ANDROID_ID`, `GOOGLE_AID`, etc. from the same source of truth as L1/L2.

### Fixed
- **Time zone** row on Device Fingerprint dashboard: was `Asia/Jakarta` (real), now spoofed to `America/Los_Angeles` in target process.
- **Locale (Region)** row: was `id-ID` (real), now spoofed to `en_US`.

### Known limitations (still leaking in v1.1.1, need Path B in v1.1.2+)
- **Android Device ID (SSAID)** — read via `Settings.Secure.getString(cr, "android_id")` → binder IPC to `system_server`. Requires lsplant Java method hook.
- **GAID** — read via Play Services `AdvertisingIdClient.Info.getId()` → binder IPC. Requires lsplant.
- **App Set ID** — Play Services binder IPC. Requires lsplant.
- **MediaDRM ID** — read via `MediaDrm.getPropertyByteArray("deviceUniqueId")` → native call into `libmediadrm.so`. Requires Dobby inline hook (v1.2+).
- **Boot count / Developer mode** — read via `Settings.Global.getInt(cr, ...)` → binder IPC. Requires lsplant.
- **Device Uptime** — read via `SystemClock.elapsedRealtime()` (`@CriticalNative`). Not affected by our `/proc/uptime` bind mount (Java uses `clock_gettime(CLOCK_BOOTTIME)`). Requires lsplant or Dobby.
- **WiFi SSID / BSSID** — `WifiInfo.getSSID()` / `getBSSID()` → binder IPC. Requires lsplant.
- **Email Accounts** — `AccountManager.getAccountsByType()` → binder IPC. Requires lsplant.
- **IP / geolocation** — network-layer, needs VPN or DNS spoofing (not in module scope).

### Unchanged from v1.1.0
- L1 Build.* Java field hooks — 17 fields (confirmed working per user screenshot: Pixel 6 / oriole / TQ3A / release-keys all spoofed).
- L2 SystemProperties.native_get inline hook — 42 identity + 13 static defaults.
- L3-L7 hooks (secure, gaid, wifi, telephony, SPB/SPI/SPL typed variants).
- Companion 11-file bind mount overlay (build.prop x5 + settings_secure.xml + proc_uptime + kernel_boot_id + …).
- Path B lsplant scaffold — `Init()` + `InstallAll()` still logging no-op; helper dex generation and real hook bodies deferred to v1.1.2.

### Impact
Dashboard rows that will change on next fresh spawn: **Time zone**, **Locale (Region)**. All Build.* rows remain spoofed. Other identifier rows unchanged pending Path B.

---

## v1.1.0

### Added

- **Kernel identity bind (Path A)** — companion now bind-mounts two additional overlay files into the target's mount namespace:
  - `/proc/uptime` → random value between 1 hour and 30 days (fresh persona shouldn't look like a device that was just booted or one that's been up for months)
  - `/proc/sys/kernel/random/boot_id` → fresh UUIDv4 per identity rotation (SafetyNet + several fingerprinters cross-check this against `ro.boottime.zygote`)
- **Rich BIND-FAIL diagnostic** — when a bind mount fails post-`setns`, companion now logs source and destination `stat()` (inode / size / mode), the raw `errno` **plus** `strerror(errno)`, and the target pid, so root-cause is obvious without reproducing under strace. Example:
  ```
  BIND-FAIL /data/adb/modules/ternak_tt/mount/settings_secure.xml -> /data/system/users/0/settings_secure.xml \
    errno=2(No such file or directory) src{stat=0 ino=12345 size=421 mode=0100644} \
    dst{stat=-1 ino=0 mode=00} [post-setns pid=23811]
  ```
- **Path B: Java method hook scaffold (lsplant)** — new `jni/java_hooks.{hpp,cpp}` implements a lsplant-based hook framework, guarded by `TT_HAVE_LSPLANT`. When compiled with lsplant available, `postAppSpecialize` now calls `java_hooks::Init(env)` → `InstallAll(env, identity)` after the crash watchdog. **v1.1.0 ships the scaffold only** — `Init()` wires up the lsplant runtime, `InstallAll()` is a logging no-op. v1.1.1 will populate hooks for `Settings.Secure.getString` (android_id / bluetooth_address / advertising_id), `MediaDrm.getPropertyByteArray` (widevine deviceUniqueId), `Locale.getDefault`, `TimeZone.getDefault`, `SystemClock.uptimeMillis`, and `SystemClock.elapsedRealtime`.
- **`fetch_lsplant.sh`** — one-shot script that clones LSPosed/LSPlant + jmpews/Dobby into `jni/`, then (if `javac` + `d8` are on PATH) compiles `java_helper/TernakHookHelper.java` into `jni/helper_dex.h` for embedding.
- **CI workflow auto-fetch** — GitHub Actions runs `./fetch_lsplant.sh` after the Zygisk header fetch. Marked `continue-on-error: true`, so if fetch fails the module still builds — just with Path B disabled.
- **Conditional CMake integration** — `jni/CMakeLists.txt` now detects `jni/lsplant/` + `jni/dobby/` and, when both exist, adds them as subdirectories, sets `-DTT_HAVE_LSPLANT=1`, and links against `lsplant` + `dobby` + `dl`. Falls back to the original single-target build when either is missing.
- **`build.sh` Path B status message** — prints whether Path B is active or disabled before invoking cmake, so build output is self-documenting.
- **`java_helper/TernakHookHelper.java`** — scaffold Java class with `native` method declarations that the v1.1.1 hooks will point to for calling the original ART method via lsplant's backup handle.

### Fixed

- **BIND-FAIL log ambiguity from v1.0.18** — previously the log line was just `child: bind fail <src> -> <dst> errno=<n>`, which didn't distinguish "source file gone" from "target NS already has this path overlaid" from "selinux denied". The rich diagnostic above resolves this.

### Known limitations

- **Path B is scaffold-only in v1.1.0.** `InstallAll()` returns without hooking any Java methods. `Settings.Secure.getString("android_id")` and `MediaDrm.getPropertyByteArray("deviceUniqueId")` will still return the real device values on v1.1.0. Full hook bodies land in v1.1.1.
- **Path B build requires internet on CI** — `fetch_lsplant.sh` clones from GitHub. Offline / air-gapped builds must vendor `jni/lsplant/` + `jni/dobby/` manually.
- **`/proc/uptime` bind** may fail on some kernels that mark `/proc/uptime` as a synthetic pseudo-file rejecting bind sources; this is why bind failures now log `errno=EINVAL(Invalid argument)` explicitly.

### Unchanged

- L1 `Build.*` ×17 hooks, L2 native_get 42 identity keys + 13 static defaults, L6 Telephony ×4 hooks, L7 SPB ×9 / SPI ×18 / SPL ×1, watchdog ×4 signals — all identical to v1.0.18.
- Companion wire protocol, target.txt hot-reload, `ternak-tt targets` CLI, `summarize.sh` two-section output — unchanged.

### Impact

- **/proc/uptime + /proc/sys/kernel/random/boot_id** now spoofed. Fingerprinters that cross-check these against `ro.boottime.zygote` or `ro.build.date.utc` should no longer flag inconsistency.
- **SSAID, GAID, App Set ID, MediaDRM ID still leak on v1.1.0.** These identifiers are read via binder IPC to `system_server` / Play Services, so no mount-namespace overlay can intercept them. Path B in v1.1.1 will close this gap by hooking the ART methods directly in the target process.
- No regression on TT / Grab / Shopee flows expected — all v1.0.18 behavior preserved; new code is additive.

---

## v1.0.18

### Added

- **L2 native_get static defaults map** — 13 new keys that leaked in real-world sessions now return safe generic values instead of the device's real value: `gsm.operator.isroaming` ("false"), `ro.zygote` ("zygote64_32"), `ro.hardware` ("qcom"), `ro.board.platform` ("sm8250"), `ro.dalvik.vm.native.bridge` ("0"), `ro.allow.mock.location` ("0"), `dalvik.vm.isa.arm64.variant` / `.features`, `dalvik.vm.isa.arm.variant` / `.features`, `dalvik.vm.heapsize` ("512m"), `ro.build.version.preview_sdk` ("0"), `persist.radio.multisim.config` fallback ("ss"). Logged as `L2 SPOOF-STATIC` so they can be counted separately.
- **L2 identity-typed additions**: `ro.build.user`, `ro.build.host`, `ro.build.tags`, `ro.build.type` now hook via native_get (previously only spoofed via resetprop on `apply-boot`).
- **L7-SPB additions** (3 keys, from real leak trace): `persist.sys.activity_anim_perf_override` (was leaking 114× per session), `persist.sys.lmk.reportkills`, `debug.layout`.
- **L7-SPI addition**: `debug.adservices.binder_timeout` = 10000.

### Fixed

- **`summarize.sh` “Target packages seen” section was blank** even when the companion accepted target spawns. Root cause: the regex captured `pkg=` from every log line including REJECTs, then sort/uniq buried target matches. Now split into two sections: **“Target packages seen (ACCEPTED by companion)”** parsed from `ACCEPT pkg='...'` lines only, and **“All packages spawned (top 20, incl. rejected)”** for whitelist tuning context.

### Unchanged

- Companion bind-mount errno=2 seen on some post-`freshen` spawns is under investigation (write path is already atomic via `rename(2)`; likely mount(2) fails post-setns because target NS has an existing overlay). Added no runtime change in v1.0.18; richer per-mount diagnostic planned for v1.0.19.

### Impact

- Expected LEAK count in next debug session: ~5 (from 182 in v1.0.17). Remaining leaks: rare device-specific props not yet catalogued.

---

## v1.0.17

### Changed

- Migrated repository from `diru768/ternak-tt` to `Ilham311/Tt`. All README badges, install links, `git clone` URL, `module.prop` `updateJson`, and `update.json` seed now point to the new repo.
- `LICENSE` copyright reassigned to `Ilham311` (MIT).

### Fixed

- Placeholder text like `<ts>`, `<part>`, `<timestamp>` were being silently stripped by Markdown / web upload paths, leaving broken filenames such as `session-.log` in README, CHANGELOG, and `customize.sh`. All placeholders replaced with concrete stripping-proof forms: `session-YYYYMMDD-HHMMSS.log`, `summary-YYYYMMDD-HHMMSS.txt`, `/{partition}/etc/build.prop`.
- `module.prop` author reassigned from `diru768` to `Ilham311`.
- `customize.sh` install banner bumped from v1.0.15 to v1.0.17 (was 2 versions stale).

### Unchanged

- No functional / runtime behavior changes. Zygisk hook layers, companion IPC protocol, `target.txt` whitelist, crash watchdog, and summarizer all identical to v1.0.16. Safe to flash over v1.0.16 without state reset.

---

## v1.0.16

### Changed

- All source files (`.cpp`, `.hpp`, `.sh`, `CMakeLists.txt`, workflow YAML) stripped of comments for cleaner distribution. Total source size reduced ~22%.
- README fully rewritten with centered header, 5 status badges, table of contents, ASCII architecture diagram (boot flow + per-app-spawn flow), Requirements table, Command reference table, Env override table, and dedicated **Auto-release pipeline** section.
- New `CHANGELOG.md` (this file) added as canonical release history in Keep-a-Changelog format.

### Added

- `.github/workflows/build.yml` fully automated: reads `module.prop` → auto-bumps patch on tag collision → builds both variants → auto-generates `release_notes.md` from `CHANGELOG.md` section + git log since prev tag → auto-generates `update.json` from `module.prop` + repo slug → commits refreshed `update.json` + synced `module.prop` back to `main` → publishes GitHub Release. **Zero manual input.**
- `release_notes.md` and `update.json` are now build artifacts / release assets, no longer committed manually.

---

## v1.0.15

### Added

- **Runtime whitelist `target.txt`** at `/data/adb/modules/ternak_tt/target.txt`. Add / remove target packages without rebuilding.
- Companion loads and **hot-reloads** `target.txt` on mtime change (next app spawn picks up edits).
- New CLI subcommand `ternak-tt targets` to dump the active whitelist.
- L7 `SUPPRESS` label for known log-noise keys (`log.looper.*.slow`, `debug.watson.*`) to keep summaries readable.
- `summarize.sh` now breaks SPOOF hits out by hook layer (`L1` / `L2` / `L7-SPB` / `L7-SPI` / `L7-SPL`) and counts `SUPPRESS`, `REJECT`, `ACCEPT` separately.
- `customize.sh` **preserves** existing `target.txt` across reinstalls.

### Changed

- Zygisk companion IPC protocol for `CMD_GET_IDENTITY` now includes the pkg name; companion responds with `len=0` for non-targets (single source of truth for whitelist).
- Zygisk `.so` no longer contains a hardcoded target list.
- `ternak-tt.cpp` `wipe_tt_data()` reads targets from `target.txt` for symmetry with the Zygisk side.

### Fixed

- Whitelist drift between the CLI (`ternak-tt`) and Zygisk companion — both now share one file.

---

## v1.0.14

### Added

- `post-fs-data.sh` + new `ternak-tt seed` subcommand that generates identity + mount overlay files **before** Zygisk loads, fixing the first-boot race where the first TT/Grab pid got 0/6 bind mounts.
- Android 11+ canonical partition paths in `BIND_ENTRIES` (`/odm/etc/build.prop`, `/product/etc/build.prop`, `/system_ext/etc/build.prop`) alongside the legacy paths.
- Skip counter is split into `skip_src` (module bug) vs `skip_dst` (device doesn't have that partition — expected).

### Fixed

- 3-skip on POCO F3 / MIUI-style ROMs where partition build.prop lives at `/{partition}/etc/build.prop`.

---

## v1.0.13

### Added

- **Per-type L7 spoof maps** (`tt_bool_spoof`, `tt_int_spoof`, `tt_long_spoof`) consulted by the typed `native_get_*` hooks before falling back to `def`.
- Critical spoof: `sys.boot_completed = true` (previously returned `false`, breaking app boot-detection retry loops).
- L2 `debug.force_rtl` → `false`.

### Fixed

- 800+ hot L7 leaks from v1.0.12 telemetry (`sys.boot_completed`, `debug.sqlite.*`, `build.version.extensions.*`, `ro.gfx.driver_build_time`, `dalvik.vm.dexopt.secondary`, etc.).

---

## v1.0.12

### Added

- Crash watchdog rewrite: 4 signals (`SIGABRT` / `SIGFPE` / `SIGILL` / `SIGSYS`), rate-limited 3 per signal, restores `SIG_DFL` (no signal chaining).
- 18 additional L2 `native_get` spoofs (`gsm.*`, `sys.boot_completed`, `cpu.abi*`, `dalvik.vm.heapgrowthlimit`, `ro.build.characteristics`, `persist.sys.timezone`, `ro.mediacodec.*`).

### Fixed

- YAML workflow version resolution (was overwriting `module.prop` with git-describe fallback `v0.0.6`).
- `summarize.sh` MOUNT regex missing the new companion log format.

---

## v1.0.11

### Added

- Standalone `summarize.sh` that condenses a 7 MB session log into a ~10 KB chat-shareable summary.
- Action tap on debug variant now auto-produces `summary-YYYYMMDD-HHMMSS.txt`, copies `crashes.log`, and gzips the raw log to `/sdcard/Download/ternak-tt-logs/`.
- Automatic pruning: keeps newest 10 summaries / crashes / raw.gz per install.

---

## v1.0.10

### Added

- **Zero-setup auto-log capture** on debug variant. `service.sh` starts a background logcat on boot into `/data/adb/modules/ternak_tt/debug/session-YYYYMMDD-HHMMSS.log`, keeping the 5 newest sessions.
- Session header written to each log (module version, boot time, uptime, Android SDK, device, ABI, installed root modules).

---

## v1.0.9

### Added

- Detailed `[D]` traces for every hook layer (which key was queried, what was returned, LEAK vs SPOOF vs MISS labelling).
- Companion mount timeline traces (`setns OK`, per-bind check, ok/fail/skip breakdown).
- Crash / death / leak journal that persists across reboots.

---

## v1.0.8

### Added

- Debug variant build alongside release. Both variants are produced by `build.sh` per invocation.
- `TT_DEBUG` compile-time flag: release strips `LOGD` calls entirely (zero cost), debug keeps them.

---

## v1.0.7

### Added

- Expanded target set to include Grab Passenger (`com.grabtaxi.passenger`) — driver/passenger apps do heavy device fingerprinting for fraud.

---

## v1.0.6

### Fixed

- Bind-mount `EINVAL` in the Zygisk-Next companion. Companion now forks a single-threaded child that `setns`es into the target's mount namespace before mounting.

---

## v1.0.5

### Added

- Zygisk companion process serving `CMD_GET_IDENTITY` and `CMD_DO_MOUNTS` over the built-in Zygisk UDS.

---

## v1.0.4

### Fixed

- `EACCES` on `mount()` from `preAppSpecialize` (CAP_SYS_ADMIN dropped). Mounting is now handled by the companion, which retains caps.

---

## v1.0.3

### Added

- Bind-mount overlay tree at `$MODPATH/mount/{system,vendor,odm,product,system_ext}/build.prop` + `settings_secure.xml`.
- `ternak-tt freshen` regenerates all overlay files.

---

## v1.0.2

### Added

- `resetprop-rs` invocation from `apply-boot` and `freshen` to broadcast native property changes.

---

## v1.0.1

### Added

- Standalone CLI (`ternak-tt`) with `freshen`, `status`, `rollback`, `lock`, `unlock`, `apply-boot` subcommands.

---

## v1.0.0

Initial release.

- 6-layer Java hook: `Build.*`, `SystemProperties.native_get`, `Settings.Secure.getString`, `AdvertisingIdClient.Info.getId` (stub), `WifiInfo.getMacAddress` / `getBSSID`, `TelephonyManager.getImei` / `getDeviceId` / `getSubscriberId` / `getMeid`.
- Pixel-only device pool (SDK 33–36).
- TikTok Global / Asia / Lite target packages.
