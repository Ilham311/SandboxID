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

set(TERNAK_TT_SOURCES main.cpp companion.cpp java_hooks.cpp)

# ============================================================================
# Path B: prebuilt lsplant (from AAR) + Dobby (from git clone)
#
# Populated by ./fetch_lsplant.sh:
#   - ../prebuilt/lsplant/lib/<ABI>/liblsplant.so   (from lsplant-standalone AAR)
#   - ../prebuilt/lsplant/include/lsplant.hpp        (from AAR)
#   - ./dobby/CMakeLists.txt                         (from jmpews/Dobby git)
#
# libternak_tt.so DT_NEEDs liblsplant.so at load time. Android's linker resolves
# DT_NEEDED via /system/lib{,64}/. build.sh ships liblsplant.so + libdobby.so
# under $MODPATH/system/lib{,64}/, which Magisk/KSU magic-mount overlays onto
# /system/lib{,64}/ at boot — so DT_NEEDED resolves inside every app process.
# ============================================================================

set(LSPLANT_PREBUILT "${CMAKE_CURRENT_SOURCE_DIR}/../prebuilt/lsplant")
set(LSPLANT_SO       "${LSPLANT_PREBUILT}/lib/${ANDROID_ABI}/liblsplant.so")
set(LSPLANT_HEADER   "${LSPLANT_PREBUILT}/include/lsplant.hpp")
set(DOBBY_DIR        "${CMAKE_CURRENT_SOURCE_DIR}/dobby")

if(EXISTS "${LSPLANT_SO}" AND EXISTS "${LSPLANT_HEADER}" AND EXISTS "${DOBBY_DIR}/CMakeLists.txt")
    message(STATUS "[ternak_tt] Path B ENABLED")
    message(STATUS "[ternak_tt]   lsplant .so:     ${LSPLANT_SO}")
    message(STATUS "[ternak_tt]   lsplant header:  ${LSPLANT_HEADER}")
    message(STATUS "[ternak_tt]   dobby dir:       ${DOBBY_DIR}")

    # Prebuilt lsplant — imported SHARED .so from AAR
    add_library(lsplant SHARED IMPORTED)
    set_target_properties(lsplant PROPERTIES
        IMPORTED_LOCATION "${LSPLANT_SO}"
        INTERFACE_INCLUDE_DIRECTORIES "${LSPLANT_PREBUILT}/include")

    # Dobby — built from source (small, no submodule issues)
    # Disable Dobby's own logging + shared build noise
    set(DOBBY_DEBUG OFF   CACHE BOOL "" FORCE)
    set(DOBBY_GENERATE_SHARED ON CACHE BOOL "" FORCE)
    add_subdirectory("${DOBBY_DIR}" dobby-build EXCLUDE_FROM_ALL)

    add_library(ternak_tt SHARED ${TERNAK_TT_SOURCES})
    target_compile_definitions(ternak_tt PRIVATE TT_HAVE_LSPLANT=1)
    target_include_directories(ternak_tt PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${LSPLANT_PREBUILT}/include)
    target_link_libraries(ternak_tt PRIVATE log dl lsplant dobby)
else()
    message(WARNING "[ternak_tt] Path B DISABLED: missing one of:")
    if(NOT EXISTS "${LSPLANT_SO}")
        message(WARNING "[ternak_tt]   - ${LSPLANT_SO}")
    endif()
    if(NOT EXISTS "${LSPLANT_HEADER}")
        message(WARNING "[ternak_tt]   - ${LSPLANT_HEADER}")
    endif()
    if(NOT EXISTS "${DOBBY_DIR}/CMakeLists.txt")
        message(WARNING "[ternak_tt]   - ${DOBBY_DIR}/CMakeLists.txt")
    endif()
    message(WARNING "[ternak_tt] Run ./fetch_lsplant.sh to populate prebuilt/lsplant + jni/dobby.")

    add_library(ternak_tt SHARED ${TERNAK_TT_SOURCES})
    target_include_directories(ternak_tt PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(ternak_tt PRIVATE log)
endif()

add_executable(ternak-tt ternak-tt.cpp)
target_include_directories(ternak-tt PRIVATE ${CMAKE_SOURCE_DIR})
