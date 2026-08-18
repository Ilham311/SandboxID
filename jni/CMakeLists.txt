cmake_minimum_required(VERSION 3.18)
project(ternak_tt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

option(TERNAK_TT_DEBUG "Enable verbose debug logging (LOGD)" OFF)

if(TERNAK_TT_DEBUG)
    message(STATUS "[ternak_tt] BUILD VARIANT: DEBUG (verbose LOGD, no strip)")
    add_compile_definitions(TT_DEBUG=1)
    set(COMMON "-Os -g -fvisibility=hidden -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti")
    set(CMAKE_CXX_FLAGS_RELEASE "${COMMON}")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "-Wl,--gc-sections -Wl,--exclude-libs,ALL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE    "-Wl,--gc-sections")
else()
    message(STATUS "[ternak_tt] BUILD VARIANT: RELEASE (LOGD compiled out, stripped)")
    set(COMMON "-Os -fvisibility=hidden -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti")
    set(CMAKE_CXX_FLAGS_RELEASE "${COMMON}")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE
        "-Wl,--gc-sections -Wl,--strip-all -Wl,--exclude-libs,ALL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE
        "-Wl,--gc-sections -Wl,--strip-all")
endif()

add_library(ternak_tt SHARED main.cpp companion.cpp)
target_include_directories(ternak_tt PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ternak_tt PRIVATE log)

add_executable(ternak-tt ternak-tt.cpp)
target_include_directories(ternak-tt PRIVATE ${CMAKE_SOURCE_DIR})

# ---- L3 (opt-in): LSPlant ART Java-method hooking. DEFAULT OFF. --------------
# ⚠️ BOOT RISK: turning this ON ships code that patches ART internals. If it
# fails on a device it can boot-loop every target app. Only enable WITH a
# build+boot test loop on each target ABI/Android version. When OFF (default),
# tt_lsplant.hpp compiles to no-op stubs and the .so is byte-identical to v1.0.27.
#
# Enabling requires vendoring under jni/external (see tt_lsplant.hpp header):
#   - LSPlant  *tag v6.4*  (C++20 + classic headers; NOT master = C++23 modules)
#   - LSPlant's external/dex_builder
#   - Dobby        (inline-hook backend: DobbyHook / DobbyDestroy)
#   - an ELF symbol resolver for libart.so (lsparself / SandHook-style ElfImg)
#   - a prebuilt callback DEX (SEAM #2 in tt_lsplant.hpp)
#
# NOTE: the COMMON flags above set -fno-exceptions/-fno-rtti via
# CMAKE_CXX_FLAGS_RELEASE (GLOBAL), so add_subdirectory targets inherit them.
# LSPlant/Dobby expect exceptions/RTTI — override per-target at integration
# (e.g. target_compile_options(lsplant_static PRIVATE -fexceptions -frtti)).
option(TT_ENABLE_LSPLANT "Enable L3 LSPlant Java-method hooks (experimental, unverified)" OFF)
if(TT_ENABLE_LSPLANT)
    message(WARNING "[ternak_tt] L3 LSPlant ENABLED — verify boot on EVERY target ABI/Android version before shipping")
    # Vendored under jni/external (opt-in; fetched by the CI "enable_lsplant" recipe
    # in CHANGELOG, or manually). Recursive submodules matter for LSPlant:
    #   external/lsplant   LSPosed/LSPlant  tag v6.4  (+ its external/dex_builder)
    #   external/dobby     inline-hook backend (jmpews/Dobby)
    #   external/lsparself libart symbol resolver (decodes .gnu_debugdata)
    # Callback DEX: jni/tt_hook_dex.h (built from jni/TtHook.java via d8) is picked
    # up by __has_include() in tt_lsplant.hpp — no extra include dir needed.
    #
    # NOTE: build with -DANDROID_STL=c++_static when enabling, so libc++ links
    # statically and no libc++_shared.so must be bundled into the module.
    set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/lsplant/lsplant/src/main/jni lsplant_build)
    add_subdirectory(external/dobby dobby_build)
    target_link_libraries(ternak_tt PRIVATE lsplant_static dobby)
    target_include_directories(ternak_tt PRIVATE
        external/lsplant/lsplant/src/main/jni/include   # lsplant.hpp
        external/lsparself                              # lsparself.hpp
        external/dobby/include)                         # dobby.h
    # The COMMON flags above set -fno-exceptions/-fno-rtti via CMAKE_CXX_FLAGS_RELEASE
    # (GLOBAL) — add_subdirectory targets inherit them. LSPlant/Dobby/dex_builder
    # require exceptions+RTTI, so override per sub-target.
    foreach(t lsplant_static dex_builder_static dobby)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE -fexceptions -frtti)
        endif()
    endforeach()
    target_compile_definitions(ternak_tt PRIVATE TT_ENABLE_LSPLANT=1)
endif()
