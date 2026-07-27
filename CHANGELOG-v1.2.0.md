## v1.2.0

**Focus**: Kill the ShadowHook `-Werror` cascade permanently by consuming ShadowHook the way ByteDance officially ships it — as an AAR from Maven Central — instead of compiling from source under a Clang that keeps promoting new warnings to errors.

### Research trail
- v1.1.9's `target_compile_options(shadowhook PRIVATE -Wno-error=...)` was expected to silence the errors but didn't fully work because ShadowHook's own CMakeLists appends `-Werror` AFTER our PRIVATE options in the final Clang command, and the LSS syscall headers use `__xxx` identifiers that `-Wreserved-identifier` catches regardless of flag ordering.
- Read ByteDance's official manual (`https://raw.githubusercontent.com/bytedance/android-inline-hook/master/doc/manual.md`): the recommended integration is via Maven Central AAR + prefab + `find_package(shadowhook REQUIRED CONFIG)`. Source builds are for developers of ShadowHook itself.
- Verified `https://repo1.maven.org/maven2/com/bytedance/android/shadowhook/`: latest release is `2.0.1` (Jun 2026), 461 KB AAR, published by ByteDance and signed. Older versions available back to 1.0.2 (Feb 2022).
- Confirmed via `bytedance/android-inline-hook/blob/main/shadowhook/build.gradle`: `prefab { shadowhook { headers = "src/main/cpp/include" } }` — same prefab package format we already handle for lsplant.

### Changed — ShadowHook: source-clone → prebuilt AAR
- **Before (v1.1.7–v1.1.9)**: `git clone bytedance/android-inline-hook@v1.0.9` into `jni/shadowhook/`, then `add_subdirectory(shadowhook/src/main/cpp)` in our CMake, then compile ~30 C files per ABI × 4 ABIs = ~120 compilation units.
- **After (v1.2.0)**: `curl` the AAR from `https://repo1.maven.org/maven2/com/bytedance/android/shadowhook/<version>/shadowhook-<version>.aar`, extract `prefab/modules/shadowhook/{include/shadowhook.h,libs/android.*/libshadowhook.so}` into `prebuilt/shadowhook/`, link as IMPORTED SHARED library. Zero source compilation.
- Auto-resolved to latest Maven `<release>` (currently `2.0.1`); override with `SHADOWHOOK_VERSION=x.y.z ./fetch_lsplant.sh`.
- ABI-compat: ByteDance docs explicitly promise API+ABI backward compat across versions, so our `shadowhook_hook_func_addr` / `shadowhook_unhook` / `shadowhook_init` calls work against 1.x and 2.x binaries identically.

### Added — SONAME rewrite via patchelf
- Problem: AAR ships `libshadowhook.so` with `SONAME=libshadowhook.so`. TikTok/Douyin bundles its own `libshadowhook.so` (ByteDance dogfoods internally). If we ship `/system/lib64/libshadowhook.so`, either TikTok's copy or ours gets shadowed at load time and ABI drift can crash the app.
- Fix: after extracting AAR, `patchelf --set-soname libternak_shadowhook.so libshadowhook.so && mv libshadowhook.so libternak_shadowhook.so` per ABI. `libternak_tt.so` then bakes `DT_NEEDED=libternak_shadowhook.so` — a namespace no TikTok/Douyin build ever touches.
- Workflow updated: `sudo apt-get install -y patchelf` step added before Path B fetch.
- Graceful degradation: if `patchelf` isn't available, `fetch_lsplant.sh` still renames the file (not SONAME) and warns; build still works but collision risk remains.

### Removed — `jni/shadowhook/` git clone + `add_subdirectory` compile
- `fetch_lsplant.sh` no longer runs `git clone --depth 1 --branch v1.0.9 https://github.com/bytedance/android-inline-hook.git jni/shadowhook/`.
- `jni/CMakeLists.txt` no longer does `add_subdirectory("${SHADOWHOOK_ROOT}/shadowhook/src/main/cpp" shadowhook-build EXCLUDE_FROM_ALL)`.
- `jni/CMakeLists.txt` no longer needs the v1.1.9 `target_compile_options(shadowhook PRIVATE -Wno-error=...)` workaround.
- `build.sh` no longer runs `find build/$V/$abi/shadowhook-build -name libternak_shadowhook.so`. Both .so files come directly from `prebuilt/{lsplant,shadowhook}/lib/$abi/`.

### Changed — `build.sh` Path B check
- Now checks `prebuilt/shadowhook/lib/` + `prebuilt/shadowhook/include/shadowhook.h` (mirroring the existing lsplant check) instead of `jni/shadowhook/shadowhook/src/main/cpp/CMakeLists.txt`.
- Prints both versions when Path B is enabled: `==> Path B: lsplant 6.4 + shadowhook 2.0.1 (AAR) -> TT_HAVE_LSPLANT will be enabled`.

### Net effect
- Zero source compilation of ShadowHook. Zero `-Werror` sensitivity to future Clang policy changes.
- ShadowHook version bump: `v1.0.9` (Jan 2024) → `2.0.1` (Jun 2026). Two years of upstream fixes for free.
- Build time faster (no more compiling ~120 ShadowHook CUs).
- No SONAME collision with target apps that bundle ShadowHook themselves.

---
