# v1.2.1: Removed -fno-exceptions -fno-rtti so java_hooks.cpp's try/catch
# around std::stoll(it->second) compiles. This bug ("cannot use 'try' with
# exceptions disabled") existed since v1.1.x but was masked by ShadowHook
# source-compile failures. Once v1.2.0 killed those (via AAR migration), the
# next compile stage revealed jni/java_hooks.cpp:98 hitting the flag mismatch.
# Trade-off: +5-10 KB per ABI for exception unwinding tables. Acceptable.
#
# v1.2.0: ShadowHook now consumed as prebuilt AAR (matching lsplant approach).
# v1.1.7-v1.1.9 tried to compile ShadowHook v1.0.9 from source via
# add_subdirectory. That broke under NDK r26d Clang with -Werror on
# -Wunsafe-buffer-usage, -Wdeclaration-after-statement, -Wreserved-identifier.
# Official ByteDance integration ships AAR + prefab on Maven Central. We use
# plain IMPORTED SHARED targets (same recipe as lsplant), skipping Gradle.
#
# v1.1.8: bump min to 3.22.1 (Android AGP standard) + add C to LANGUAGES.
cmake_minimum_required(VERSION 3.22.1)
project(ternak_tt LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)

option(TERNAK_TT_DEBUG "Enable verbose debug logging (LOGD)" OFF)

if(TERNAK_TT_DEBUG)
    message(STATUS "[ternak_tt] BUILD VARIANT: DEBUG (verbose LOGD, no strip)")
    add_compile_definitions(TT_DEBUG=1)
    # v1.2.1: -fno-exceptions -fno-rtti removed (java_hooks.cpp uses try/catch)
    set(COMMON "-Os -g -fvisibility=hidden -ffunction-sections -fdata-sections")
    set(CMAKE_CXX_FLAGS_RELEASE "${COMMON}")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "-Wl,--gc-sections -Wl,--exclude-libs,ALL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE    "-Wl,--gc-sections")
else()
    message(STATUS "[ternak_tt] BUILD VARIANT: RELEASE (LOGD compiled out, stripped)")
    # v1.2.1: -fno-exceptions -fno-rtti removed (java_hooks.cpp uses try/catch)
    set(COMMON "-Os -fvisibility=hidden -ffunction-sections -fdata-sections")
    set(CMAKE_CXX_FLAGS_RELEASE "${COMMON}")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE
        "-Wl,--gc-sections -Wl,--strip-all -Wl,--exclude-libs,ALL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE
        "-Wl,--gc-sections -Wl,--strip-all")
endif()

set(TERNAK_TT_SOURCES main.cpp companion.cpp java_hooks.cpp)

# ============================================================================
# Path B: prebuilt lsplant + shadowhook (both from Maven Central AARs)
#
# Populated by ./fetch_lsplant.sh:
#   ../prebuilt/lsplant/lib/<ABI>/liblsplant.so
#   ../prebuilt/lsplant/include/lsplant.hpp
#   ../prebuilt/shadowhook/lib/<ABI>/libternak_shadowhook.so  (SONAME rewritten)
#   ../prebuilt/shadowhook/include/shadowhook.h
#
# libternak_tt.so DT_NEEDs both at load time. Android's linker resolves
# DT_NEEDED via /system/lib{,64}/. build.sh ships both .so files under
# $MODPATH/system/lib{,64}/, which Magisk/KSU magic-mount overlays onto
# /system/lib{,64}/ at boot — so DT_NEEDED resolves inside every app process.
# ============================================================================

set(LSPLANT_PREBUILT    "${CMAKE_CURRENT_SOURCE_DIR}/../prebuilt/lsplant")
set(LSPLANT_SO          "${LSPLANT_PREBUILT}/lib/${ANDROID_ABI}/liblsplant.so")
set(LSPLANT_HEADER      "${LSPLANT_PREBUILT}/include/lsplant.hpp")

set(SHADOWHOOK_PREBUILT "${CMAKE_CURRENT_SOURCE_DIR}/../prebuilt/shadowhook")
set(SHADOWHOOK_SO       "${SHADOWHOOK_PREBUILT}/lib/${ANDROID_ABI}/libternak_shadowhook.so")
set(SHADOWHOOK_HEADER   "${SHADOWHOOK_PREBUILT}/include/shadowhook.h")

if(EXISTS "${LSPLANT_SO}" AND EXISTS "${LSPLANT_HEADER}"
   AND EXISTS "${SHADOWHOOK_SO}" AND EXISTS "${SHADOWHOOK_HEADER}")
    message(STATUS "[ternak_tt] Path B ENABLED")
    message(STATUS "[ternak_tt]   lsplant .so:       ${LSPLANT_SO}")
    message(STATUS "[ternak_tt]   lsplant header:    ${LSPLANT_HEADER}")
    message(STATUS "[ternak_tt]   shadowhook .so:    ${SHADOWHOOK_SO}")
    message(STATUS "[ternak_tt]   shadowhook header: ${SHADOWHOOK_HEADER}")

    add_library(lsplant SHARED IMPORTED)
    set_target_properties(lsplant PROPERTIES
        IMPORTED_LOCATION "${LSPLANT_SO}"
        INTERFACE_INCLUDE_DIRECTORIES "${LSPLANT_PREBUILT}/include")

    add_library(shadowhook SHARED IMPORTED)
    set_target_properties(shadowhook PROPERTIES
        IMPORTED_LOCATION "${SHADOWHOOK_SO}"
        INTERFACE_INCLUDE_DIRECTORIES "${SHADOWHOOK_PREBUILT}/include")

    add_library(ternak_tt SHARED ${TERNAK_TT_SOURCES})
    target_compile_definitions(ternak_tt PRIVATE TT_HAVE_LSPLANT=1)
    target_include_directories(ternak_tt PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${LSPLANT_PREBUILT}/include
        ${SHADOWHOOK_PREBUILT}/include)
    target_link_libraries(ternak_tt PRIVATE log dl lsplant shadowhook)
else()
    message(WARNING "[ternak_tt] Path B DISABLED: missing one of:")
    if(NOT EXISTS "${LSPLANT_SO}")
        message(WARNING "[ternak_tt]   - ${LSPLANT_SO}")
    endif()
    if(NOT EXISTS "${LSPLANT_HEADER}")
        message(WARNING "[ternak_tt]   - ${LSPLANT_HEADER}")
    endif()
    if(NOT EXISTS "${SHADOWHOOK_SO}")
        message(WARNING "[ternak_tt]   - ${SHADOWHOOK_SO}")
    endif()
    if(NOT EXISTS "${SHADOWHOOK_HEADER}")
        message(WARNING "[ternak_tt]   - ${SHADOWHOOK_HEADER}")
    endif()
    message(WARNING "[ternak_tt] Run ./fetch_lsplant.sh to populate prebuilt/.")

    add_library(ternak_tt SHARED ${TERNAK_TT_SOURCES})
    target_include_directories(ternak_tt PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(ternak_tt PRIVATE log)
endif()

add_executable(ternak-tt ternak-tt.cpp)
target_include_directories(ternak-tt PRIVATE ${CMAKE_SOURCE_DIR})
