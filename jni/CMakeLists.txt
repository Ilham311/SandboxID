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

# v1.1.0: mount_targets.hpp + prop_map.hpp are header-only and included by main.cpp/companion.cpp/ternak-tt.cpp.
add_library(ternak_tt SHARED main.cpp companion.cpp)
target_include_directories(ternak_tt PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ternak_tt PRIVATE log)

add_executable(ternak-tt ternak-tt.cpp)
target_include_directories(ternak-tt PRIVATE ${CMAKE_SOURCE_DIR})
