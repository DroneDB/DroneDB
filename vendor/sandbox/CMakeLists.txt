# sandbox: confinement layers for the helper programs libddb starts (rlimits, seccomp-bpf and
# Landlock on Linux, a Job Object and a low integrity level on Windows). A static library that
# depends on nothing of libddb, so every helper program (gdal-util, metadata-util, ...) links it;
# its header is C++17 and it builds standalone too.
cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(sandbox LANGUAGES CXX)
endif()

set(DDB_SANDBOX_SOURCES src/sandbox.cpp)
if(WIN32)
    list(APPEND DDB_SANDBOX_SOURCES src/platform_windows.cpp)
else()
    list(APPEND DDB_SANDBOX_SOURCES src/rlimits.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND DDB_SANDBOX_SOURCES src/platform_linux.cpp src/seccomp_linux.cpp src/landlock_linux.cpp)
    else()
        list(APPEND DDB_SANDBOX_SOURCES src/platform_other.cpp)
    endif()
endif()

add_library(ddb_sandbox STATIC ${DDB_SANDBOX_SOURCES})
target_include_directories(ddb_sandbox PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(ddb_sandbox PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
if(WIN32)
    target_link_libraries(ddb_sandbox PRIVATE advapi32)
    target_compile_definitions(ddb_sandbox PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(ddb_sandbox PRIVATE /utf-8)
    endif()
endif()
# The warning flags of the DroneDB build, when this directory is part of it
if(TARGET ddb_warnings)
    target_link_libraries(ddb_sandbox PRIVATE ddb_warnings)
endif()
