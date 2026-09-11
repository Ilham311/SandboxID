cmake_minimum_required(VERSION 3.18)
project(sandboxid LANGUAGES CXX)

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

add_library(sandboxid_module SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/../jni/module.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../jni/companion.cpp)
set_target_properties(sandboxid_module PROPERTIES OUTPUT_NAME sandboxid)
target_include_directories(sandboxid_module PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../jni)
target_link_libraries(sandboxid_module PRIVATE log)

add_executable(sandboxid sandboxid.cpp)
target_include_directories(sandboxid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
