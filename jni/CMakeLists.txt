cmake_minimum_required(VERSION 3.18)
project(sandboxid LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)

option(SBX_DEBUG "Enable verbose debug logging (LOGD)" OFF)

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

add_library(sandboxid_module SHARED main.cpp companion.cpp)
set_target_properties(sandboxid_module PROPERTIES OUTPUT_NAME sandboxid)
target_include_directories(sandboxid_module PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(sandboxid_module PRIVATE log)

add_executable(sandboxid sandboxid.cpp)
target_include_directories(sandboxid PRIVATE ${CMAKE_SOURCE_DIR})

if(DEFINED SBX_MODULE_ID AND NOT SBX_MODULE_ID STREQUAL "")
    message(STATUS "[sandboxid] module id: ${SBX_MODULE_ID}")
    target_compile_definitions(sandboxid_module PRIVATE SBX_MODULE_ID="${SBX_MODULE_ID}")
    target_compile_definitions(sandboxid        PRIVATE SBX_MODULE_ID="${SBX_MODULE_ID}")
endif()

option(SBX_BUILD_TESTS "Build host unit tests (tests/*.cpp)" OFF)
if(SBX_BUILD_TESTS)
    if(CMAKE_CROSSCOMPILING)
        message(WARNING "[sandboxid] SBX_BUILD_TESTS=ON while cross-compiling — tests will be built but cannot run on the host")
    endif()
    enable_testing()
    add_executable(sbx_carrier_test ${CMAKE_SOURCE_DIR}/../tests/carrier_test.cpp)
    add_executable(sbx_native_read_test ${CMAKE_SOURCE_DIR}/../tests/native_read_test.cpp)
    add_test(NAME carrier_test     COMMAND sbx_carrier_test)
    add_test(NAME native_read_test COMMAND sbx_native_read_test)
endif()

option(SBX_ENABLE_LSPLANT "Enable L3 LSPlant Java-method hooks (ON by default; set -DSBX_ENABLE_LSPLANT=OFF to disable)" ON)
if(SBX_ENABLE_LSPLANT)
    message(WARNING "[sandboxid] L3 LSPlant ENABLED — verify boot on EVERY target ABI/Android version before shipping")

    set(SBX_EXT     "${CMAKE_SOURCE_DIR}/external")
    set(SBX_LSP_JNI "${SBX_EXT}/lsplant/lsplant/src/main/jni")

    if(NOT EXISTS "${SBX_LSP_JNI}/CMakeLists.txt"
       OR NOT EXISTS "${SBX_EXT}/dobby/CMakeLists.txt"
       OR NOT EXISTS "${SBX_EXT}/lsparself/lsparself.hpp")
        message(FATAL_ERROR
            "[sandboxid] L3 dependencies missing under ${SBX_EXT}.\n"
            "  Run:  bash ${CMAKE_SOURCE_DIR}/fetch_lsplant_deps.sh\n"
            "  (vendors LSPlant + Dobby + lsparself; see prebuilt/lsplant/README.md)")
    endif()

    set(SBX_XZ "${SBX_EXT}/xz")
    if(NOT EXISTS "${SBX_XZ}/linux/lib/xz/xz_dec_stream.c"
       OR NOT EXISTS "${SBX_XZ}/linux/include/linux/xz.h"
       OR NOT EXISTS "${SBX_XZ}/userspace/xz_config.h")
        message(FATAL_ERROR
            "[sandboxid] xz-embedded missing under ${SBX_XZ}.\n"
            "  Run:  bash ${CMAKE_SOURCE_DIR}/fetch_lsplant_deps.sh")
    endif()
    add_library(sbx_xz STATIC
        "${SBX_XZ}/linux/lib/xz/xz_crc32.c"
        "${SBX_XZ}/linux/lib/xz/xz_crc64.c"
        "${SBX_XZ}/linux/lib/xz/xz_dec_stream.c"
        "${SBX_XZ}/linux/lib/xz/xz_dec_lzma2.c")
    target_include_directories(sbx_xz PUBLIC "${SBX_XZ}/linux/include/linux")
    target_include_directories(sbx_xz PRIVATE
        "${SBX_XZ}/linux/lib/xz" "${SBX_XZ}/userspace")
    target_compile_definitions(sbx_xz PUBLIC  XZ_USE_CRC64)
    target_compile_definitions(sbx_xz PRIVATE XZ_DEC_ANY_CHECK)
    set_target_properties(sbx_xz PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
    target_compile_options(sbx_xz PRIVATE -fno-lto)

    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/hook_dex.h")
        message(STATUS "[sandboxid] generating hook_dex.h (LSPlant callback DEX)")
        execute_process(
            COMMAND bash "${CMAKE_SOURCE_DIR}/tools/gen_hook_dex.sh"
            RESULT_VARIABLE SBX_DEXGEN_RC
            OUTPUT_VARIABLE SBX_DEXGEN_LOG
            ERROR_VARIABLE  SBX_DEXGEN_LOG)
        if(NOT SBX_DEXGEN_RC EQUAL 0)
            message(WARNING
                "[sandboxid] hook_dex.h generation failed — the L3 ANDROID_ID hook "
                "will be SKIPPED at runtime (L1/L2 unaffected). Install a JDK + Android "
                "SDK build-tools, then run: bash ${CMAKE_SOURCE_DIR}/tools/gen_hook_dex.sh\n"
                "${SBX_DEXGEN_LOG}")
        endif()
    endif()

    set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
    add_subdirectory("${SBX_LSP_JNI}" lsplant_build)
    add_subdirectory("${SBX_EXT}/dobby" dobby_build)
    target_link_libraries(sandboxid_module PRIVATE lsplant_static dobby sbx_xz)
    target_include_directories(sandboxid_module PRIVATE
        "${SBX_LSP_JNI}/include"
        "${SBX_EXT}/lsparself"
        "${SBX_EXT}/dobby/include")
    foreach(t lsplant_static dex_builder_static dobby)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE -fexceptions -frtti -fno-lto)
        endif()
    endforeach()
    target_compile_definitions(sandboxid_module PRIVATE SBX_ENABLE_LSPLANT=1)
    if(NOT SBX_DEBUG)
        set_target_properties(sandboxid_module PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
        target_link_options(sandboxid_module PRIVATE -fno-lto)
    endif()
endif()
