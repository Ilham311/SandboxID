cmake_minimum_required(VERSION 3.18)
project(sandboxid LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

option(SBX_DEBUG "Enable verbose debug logging (LOGD)" OFF)
option(SBX_ENABLE_LSPLANT
       "Build the L3 Java-hook layer (LSPlant + Dobby from jni/external)" OFF)

if(SBX_DEBUG)
    message(STATUS "[sandboxid] BUILD VARIANT: DEBUG (verbose LOGD, no strip)")
    add_compile_definitions(SBX_DEBUG=1)
    set(COMMON "-Os -g -Wall -Wextra -fvisibility=hidden -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti")
    set(CMAKE_CXX_FLAGS_RELEASE "${COMMON}")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "-Wl,--gc-sections -Wl,--exclude-libs,ALL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE    "-Wl,--gc-sections")
else()
    message(STATUS "[sandboxid] BUILD VARIANT: RELEASE (LOGD compiled out, stripped, LTO)")
    set(COMMON "-Os -Wall -Wextra -flto -fvisibility=hidden -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti")
    set(CMAKE_CXX_FLAGS_RELEASE "${COMMON}")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE
        "-flto -Wl,--gc-sections -Wl,--strip-all -Wl,--exclude-libs,ALL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE
        "-flto -Wl,--gc-sections -Wl,--strip-all")
endif()

add_library(sandboxid_module SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/../jni/module.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../jni/companion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../jni/module_hooks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../jni/prop_hooks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../jni/typed_props.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../jni/uptime_hook.cpp
    native_read.cpp)
set_target_properties(sandboxid_module PROPERTIES OUTPUT_NAME sandboxid)
target_include_directories(sandboxid_module PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../jni)
target_link_libraries(sandboxid_module PRIVATE log)

add_executable(sandboxid sandboxid.cpp native_read.cpp)
target_include_directories(sandboxid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# =============================================================================
# L3: Java-method hooks (LSPlant + Dobby), statically linked
# =============================================================================
#
# Both dependencies are compiled from source into this one shared object. See
# jni/sbx_lsplant.hpp for why a prebuilt liblsplant.so is not used: the app
# process cannot read /data/adb/modules, and Zygisk loads only zygisk/<abi>.so.
#
# `--exclude-libs,ALL` on the module's own link line is what keeps the third
# party code from being re-exported; that is already set for both variants above
# and matters more now that two whole libraries are linked in.
if(SBX_ENABLE_LSPLANT)
    set(SBX_EXT "${CMAKE_CURRENT_SOURCE_DIR}/../jni/external")
    set(SBX_LSP_JNI "${SBX_EXT}/lsplant/lsplant/src/main/jni")

    if(NOT EXISTS "${SBX_LSP_JNI}/CMakeLists.txt"
       OR NOT EXISTS "${SBX_EXT}/dobby/CMakeLists.txt"
       OR NOT EXISTS "${SBX_LSP_JNI}/external/dex_builder/CMakeLists.txt")
        message(FATAL_ERROR
            "[sandboxid] SBX_ENABLE_LSPLANT=ON but the pinned sources are missing.\n"
            "  Expected: ${SBX_LSP_JNI}/CMakeLists.txt\n"
            "            ${SBX_EXT}/dobby/CMakeLists.txt\n"
            "            ${SBX_LSP_JNI}/external/dex_builder/CMakeLists.txt\n"
            "  Run: bash jni/fetch_lsplant_deps.sh")
    endif()

    # The callback class is shipped as DEX, generated from jni/SandboxIDHook.java.
    # Generated once and cached; delete jni/hook_dex.h to force a rebuild after
    # editing the Java source.
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../jni/hook_dex.h")
        message(STATUS "[sandboxid] generating jni/hook_dex.h")
        execute_process(
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../jni/tools/gen_hook_dex.sh"
            RESULT_VARIABLE SBX_DEX_RC)
        if(NOT SBX_DEX_RC EQUAL 0 OR NOT EXISTS
               "${CMAKE_CURRENT_SOURCE_DIR}/../jni/hook_dex.h")
            message(FATAL_ERROR
                "[sandboxid] jni/tools/gen_hook_dex.sh failed (rc=${SBX_DEX_RC}).\n"
                "  It needs javac and d8 (Android build-tools >= 28) on PATH or via\n"
                "  JAVA_HOME / D8 / ANDROID_HOME.")
        endif()
    endif()

    # CMake 4 removed compatibility with cmake_minimum_required(<3.5), and both
    # LSPlant v6.4 and DexBuilder declare 3.4.1. Without this the configure dies
    # in the subproject before a single file is compiled. The value only raises
    # the floor CMake applies to those two declarations; it does not change what
    # they build.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    endif()

    # The module's release flags are tuned for OUR code (-Os, -flto, hidden
    # visibility, -fno-exceptions -fno-rtti). Letting them leak into Dobby and
    # LSPlant would run third-party code through LTO with a visibility setting it
    # does not expect, so the subdirectories get a plain -O2 instead. The scope
    # copy that add_subdirectory() takes is what makes this local.
    set(_sbx_saved_cxx_rel "${CMAKE_CXX_FLAGS_RELEASE}")
    set(_sbx_saved_c_rel   "${CMAKE_C_FLAGS_RELEASE}")
    set(CMAKE_CXX_FLAGS_RELEASE "-O2")
    set(CMAKE_C_FLAGS_RELEASE   "-O2")

    # ---- Dobby: the inline hook engine LSPlant drives ----
    # Only the static target is linked. Its SHARED sibling would produce a
    # second libdobby.so that nothing loads; upstream's defaults are kept
    # otherwise so the build stays on the configuration Dobby tests.
    add_subdirectory("${SBX_EXT}/dobby" "${CMAKE_CURRENT_BINARY_DIR}/dobby_build")

    # Dobby always declares BOTH a SHARED and a STATIC target, and the SHARED
    # one does not link on ELF: its arm64 closure bridge asm references
    # common_closure_bridge_handler, which upstream declares `inline` in a
    # header, so with -fvisibility=hidden the shared link leaves it undefined.
    # Only dobby_static is linked into this module, so keep the SHARED sibling
    # out of `all` — otherwise a plain `cmake --build` fails on a target nothing
    # consumes.
    if(TARGET dobby)
        set_target_properties(dobby PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()

    # ---- LSPlant ----
    # LSPLANT_BUILD_SHARED=OFF keeps CMake from producing a liblsplant.so whose
    # POST_BUILD step runs objcopy/strip against a DEBUG_SYMBOLS_PATH we do not
    # want; the static target is what gets linked.
    set(LSPLANT_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(LSPLANT_STANDALONE   OFF CACHE BOOL "" FORCE)
    add_subdirectory("${SBX_LSP_JNI}" "${CMAKE_CURRENT_BINARY_DIR}/lsplant_build")

    set(CMAKE_CXX_FLAGS_RELEASE "${_sbx_saved_cxx_rel}")
    set(CMAKE_C_FLAGS_RELEASE   "${_sbx_saved_c_rel}")

    target_include_directories(sandboxid_module PRIVATE
        "${SBX_LSP_JNI}/include"
        "${SBX_EXT}/dobby/include")
    target_link_libraries(sandboxid_module PRIVATE lsplant_static dobby_static z)
    target_compile_definitions(sandboxid_module PRIVATE SBX_ENABLE_LSPLANT=1)

    # LSPlant's public header builds its callbacks out of std::function, which
    # compiles fine under -fno-exceptions -fno-rtti (libc++ aborts instead of
    # throwing), so the module keeps the same flags as the rest of its TUs rather
    # than mixing exception models across one shared object.
endif()
