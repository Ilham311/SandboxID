# `prebuilt/lsplant/` — L3 LSPlant / Dobby vendoring notes

**There are no runtime prebuilt shared libraries to ship here.** This directory
exists only to document how the L3 (LSPlant + Dobby) dependency stack is wired,
because the layout in the task spec (`prebuilt/lsplant/`) does not match how the
native build actually consumes these libraries.

## Why there is no `.so` here

LSPlant and Dobby are **statically linked** into the single Zygisk module
`libsandboxid.so`. See `jni/CMakeLists.txt`:

```cmake
target_link_libraries(sandboxid_module PRIVATE lsplant_static dobby)
```

`lsplant_static` and `dobby` are static archives (`.a`) produced from source at
build time and folded into `libsandboxid.so`. Nothing extra is loaded at
runtime, so the module ships exactly one native artifact per ABI
(`zygisk/<abi>.so`) — the same as a build without L3. Shipping a separate
`liblsplant.so` / `libdobby.so` would only add surface area and load-order risk.

## Where the sources come from

The static libraries are compiled from source trees vendored into
`jni/external/` (git-ignored, never committed). Populate them with:

```sh
jni/fetch_lsplant_deps.sh      # clones LSPlant, Dobby, lsparself into jni/external/
```

Expected layout after fetching (matches the paths hard-coded in
`jni/CMakeLists.txt`):

```
jni/external/
├── lsplant/lsplant/src/main/jni/     # LSPlant v2.0 (add_subdirectory target: lsplant_static)
│   └── include/lsplant.hpp
├── dobby/                            # Dobby (add_subdirectory target: dobby)
│   └── include/dobby.h
└── lsparself/
    └── lsparself.hpp                 # libart.so symbol resolver used by jni/sbx_lsplant.hpp
```

## Generated callback DEX

The Java hook callback (`jni/EnvCompatState.java` → class
`androidx.core.os.EnvCompatState`) must be compiled to a DEX and embedded as a C
header (`jni/hook_dex.h`, also git-ignored). Generate it with:

```sh
jni/tools/gen_hook_dex.sh          # javac + d8 -> jni/hook_dex.h
```

`build.sh` runs both scripts automatically when `SBX_ENABLE_LSPLANT=ON`.

See the repository `README.md` (“Per-app `ANDROID_ID` needs the L3 hook”) and
`jni/sbx_lsplant.hpp` for the full picture.
