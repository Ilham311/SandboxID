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

# Path B: optional lsplant + Dobby subdirs (populated by ./fetch_lsplant.sh)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lsplant/lsplant/src/main/jni/CMakeLists.txt"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dobby/CMakeLists.txt")
    message(STATUS "[ternak_tt] Path B: lsplant + dobby found -> TT_HAVE_LSPLANT=1")
    add_subdirectory(dobby   dobby-build   EXCLUDE_FROM_ALL)
    add_subdirectory(lsplant/lsplant/src/main/jni lsplant-build EXCLUDE_FROM_ALL)
    add_library(ternak_tt SHARED ${TERNAK_TT_SOURCES})
    target_compile_definitions(ternak_tt PRIVATE TT_HAVE_LSPLANT=1)
    target_include_directories(ternak_tt PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/lsplant/lsplant/src/main/jni/include)
    target_link_libraries(ternak_tt PRIVATE log lsplant dobby dl)
else()
    message(WARNING "[ternak_tt] Path B disabled: lsplant/ or dobby/ missing. Run ./fetch_lsplant.sh to enable Java method hooks.")
    add_library(ternak_tt SHARED ${TERNAK_TT_SOURCES})
    target_include_directories(ternak_tt PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(ternak_tt PRIVATE log)
endif()

add_executable(ternak-tt ternak-tt.cpp)
target_include_directories(ternak-tt PRIVATE ${CMAKE_SOURCE_DIR})
