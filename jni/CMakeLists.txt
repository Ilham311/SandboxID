cmake_minimum_required(VERSION 3.18)
project(ternak_tt LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)

set(COMMON "-Os -fvisibility=hidden -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_RELEASE "${COMMON} -fno-exceptions -fno-rtti")
set(CMAKE_C_FLAGS_RELEASE   "${COMMON}")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE
    "-Wl,--gc-sections -Wl,--strip-all -Wl,--exclude-libs,ALL")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE
    "-Wl,--gc-sections -Wl,--strip-all")

add_library(ternak_tt SHARED main.cpp companion.cpp)
target_include_directories(ternak_tt PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ternak_tt PRIVATE log)

add_executable(ternak-tt ternak-tt.c)
