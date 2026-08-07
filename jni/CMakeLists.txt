









cmake_minimum_required(VERSION 3.22)
project(ternak_tt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TERNAK_TT_DEBUG "Enable verbose debug logging (LOGD)" OFF)
option(TERNAK_TT_ASAN  "Enable AddressSanitizer in DEBUG builds" OFF)


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


add_compile_options(
    -Wall -Wextra -Wpedantic
    -Werror=return-type
    -Werror=uninitialized
    -Wno-unused-parameter
    -Wno-c99-designator
)


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




include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/improvements
)


add_library(ternak_tt SHARED main.cpp companion.cpp)
target_include_directories(ternak_tt PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ternak_tt PRIVATE log)


add_executable(ternak-tt ternak-tt.cpp)
target_include_directories(ternak-tt PRIVATE ${CMAKE_SOURCE_DIR})
