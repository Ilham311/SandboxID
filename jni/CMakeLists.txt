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

add_library(ternak_tt SHARED main.cpp companion.cpp)
target_include_directories(ternak_tt PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ternak_tt PRIVATE log)

add_executable(ternak-tt ternak-tt.cpp)
target_include_directories(ternak-tt PRIVATE ${CMAKE_SOURCE_DIR})

option(TT_ENABLE_LSPLANT "Enable L3 LSPlant Java-method hooks (experimental, unverified)" OFF)
if(TT_ENABLE_LSPLANT)
    message(WARNING "[ternak_tt] L3 LSPlant ENABLED — verify boot on EVERY target ABI/Android version before shipping")
    set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/lsplant/lsplant/src/main/jni lsplant_build)
    add_subdirectory(external/dobby dobby_build)
    target_link_libraries(ternak_tt PRIVATE lsplant_static dobby)
    target_include_directories(ternak_tt PRIVATE
        external/lsplant/lsplant/src/main/jni/include
        external/lsparself
        external/dobby/include)
    foreach(t lsplant_static dex_builder_static dobby)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE -fexceptions -frtti)
        endif()
    endforeach()
    target_compile_definitions(ternak_tt PRIVATE TT_ENABLE_LSPLANT=1)
endif()
