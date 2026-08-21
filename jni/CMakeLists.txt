cmake_minimum_required(VERSION 3.18)
project(sandboxid LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

option(SBX_DEBUG "Enable verbose debug logging (LOGD)" OFF)

if(SBX_DEBUG)
    message(STATUS "[sandboxid] BUILD VARIANT: DEBUG (verbose LOGD, no strip)")
    add_compile_definitions(SBX_DEBUG=1)
    # M8: -Wall -Wextra untuk menangkap bug diam (unused/shadow/sign) saat dev.
    set(COMMON "-Os -g -Wall -Wextra -fvisibility=hidden -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti")
    set(CMAKE_CXX_FLAGS_RELEASE "${COMMON}")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "-Wl,--gc-sections -Wl,--exclude-libs,ALL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE    "-Wl,--gc-sections")
else()
    message(STATUS "[sandboxid] BUILD VARIANT: RELEASE (LOGD compiled out, stripped, LTO)")
    # M8: -Wall -Wextra + LTO (-flto) untuk kode lebih kecil & inlining lintas-TU.
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

option(SBX_ENABLE_LSPLANT "Enable L3 LSPlant Java-method hooks (experimental, unverified)" OFF)
if(SBX_ENABLE_LSPLANT)
    message(WARNING "[sandboxid] L3 LSPlant ENABLED — verify boot on EVERY target ABI/Android version before shipping")
    set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/lsplant/lsplant/src/main/jni lsplant_build)
    add_subdirectory(external/dobby dobby_build)
    target_link_libraries(sandboxid_module PRIVATE lsplant_static dobby)
    target_include_directories(sandboxid_module PRIVATE
        external/lsplant/lsplant/src/main/jni/include
        external/lsparself
        external/dobby/include)
    foreach(t lsplant_static dex_builder_static dobby)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE -fexceptions -frtti)
        endif()
    endforeach()
    target_compile_definitions(sandboxid_module PRIVATE SBX_ENABLE_LSPLANT=1)
endif()
