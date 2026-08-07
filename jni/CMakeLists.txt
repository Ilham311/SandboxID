# ternak_tt v2.2 - CMakeLists.txt (Principal Architect refactor)
#
# Changes vs v2.1:
#   P2-F: Release now enables IPO/LTO (link-time optimization) and PIE, and
#         promotes -Wunused-result to a hard error so callers can't silently
#         ignore write() / read() return values (a real ndk-jni-tips warning).
#   P2-F: -Wno-c99-designator was clang-only; dropped so we don't emit a gcc
#         'unrecognized option' notice when a host toolchain runs syntax-check.
#   Docs: refreshed header + inline rationale comments.

cmake_minimum_required(VERSION 3.22)
project(ternak_tt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TERNAK_TT_DEBUG "Enable verbose debug logging (LOGD)" OFF)
option(TERNAK_TT_ASAN  "Enable AddressSanitizer in DEBUG builds" OFF)

# --- Item #3: auto-inject version from ../module.prop ------------------
set(TT_MODULE_PROP "${CMAKE_SOURCE_DIR}/../module.prop")
if(EXISTS "${TT_MODULE_PROP}")
    file(READ "${TT_MODULE_PROP}" _MODPROP)
    string(REGEX MATCH "version=v?([^\r\n]+)" _ "${_MODPROP}")
    set(TT_VERSION_STR "${CMAKE_MATCH_1}")
    string(REGEX MATCH "versionCode=([0-9]+)" _ "${_MODPROP}")
    set(TT_VERSION_CODE "${CMAKE_MATCH_1}")
endif()
if(NOT TT_VERSION_STR)
    set(TT_VERSION_STR "0.0.0-unknown")
endif()
if(NOT TT_VERSION_CODE)
    set(TT_VERSION_CODE "0")
endif()
message(STATUS "[ternak_tt] TT_VERSION_STR=${TT_VERSION_STR} TT_VERSION_CODE=${TT_VERSION_CODE}")

add_compile_definitions(
    TT_VERSION_STR="${TT_VERSION_STR}"
    TT_VERSION_CODE=${TT_VERSION_CODE}
    _FILE_OFFSET_BITS=64
)

# --- Item #11: enable warnings ----------------------------------------
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Werror=return-type
    -Werror=uninitialized
    -Werror=unused-result    # v2.2 P2-F: never silently drop read/write result
    -Wno-unused-parameter    # Zygisk callbacks intentionally ignore some
)

# v2.2 P2-F: build all objects as position-independent code. Zygisk requires
# PIE for the injected .so, and the standalone CLI benefits from PIE too
# (ASLR hardening on Android 10+).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# v2.2 P2-F: enable link-time optimization on non-debug builds. Reduces .so
# size ~15% and inlines the L2/L7 hook fast paths across TU boundaries.
if(NOT TERNAK_TT_DEBUG)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
    if(_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        message(STATUS "[ternak_tt] LTO (IPO) enabled for Release")
    else()
        message(STATUS "[ternak_tt] LTO not supported by toolchain: ${_ipo_msg}")
    endif()
endif()

# Common size-optimized flags
set(TT_COMMON_FLAGS
    -Os -fvisibility=hidden -ffunction-sections -fdata-sections
    -fno-exceptions -fno-rtti)
set(TT_COMMON_LINK
    -Wl,--gc-sections -Wl,--exclude-libs,ALL)

if(TERNAK_TT_DEBUG)
    message(STATUS "[ternak_tt] BUILD VARIANT: DEBUG (verbose LOGD, no strip)")
    add_compile_definitions(TT_DEBUG=1)
    add_compile_options(${TT_COMMON_FLAGS} -g)
    add_link_options(${TT_COMMON_LINK})

    if(TERNAK_TT_ASAN)
        message(STATUS "[ternak_tt] AddressSanitizer ENABLED")
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
else()
    message(STATUS "[ternak_tt] BUILD VARIANT: RELEASE (LOGD compiled out, stripped)")
    add_compile_options(${TT_COMMON_FLAGS})
    add_link_options(${TT_COMMON_LINK} -Wl,--strip-all)
endif()

# --- Include paths ---------------------------------------------------
# improvements/ is a sibling folder shipped with the patch; source files
# reference its headers by short name (no "improvements/" prefix required).
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/improvements
)

# --- Zygisk shared library ------------------------------------------
add_library(ternak_tt SHARED main.cpp companion.cpp)
target_include_directories(ternak_tt PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ternak_tt PRIVATE log)

# --- Standalone CLI -------------------------------------------------
add_executable(ternak-tt ternak-tt.cpp)
target_include_directories(ternak-tt PRIVATE ${CMAKE_SOURCE_DIR})
