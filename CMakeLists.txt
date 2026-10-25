cmake_minimum_required(VERSION 3.21)
project(ddb LANGUAGES CXX C)

# C++20. CMAKE_CXX_EXTENSIONS is left at its default.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_MESSAGE_LOG_LEVEL VERBOSE)

message(STATUS "CMAKE_BINARY_DIR: ${CMAKE_BINARY_DIR}")

# Set default build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE Release)
else()
    message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
endif()

# Build tree layout: everything users and tests run, and everything packaging copies, is in the
# build root (ddbcmd, libddb, libnxs, ddbtest and its helpers, the data files and, on Windows,
# the DLLs). Targets are built there directly instead of being copied after the
# build, so build/Makefile and build/CTestTestfile.cmake stay the top-level ones. The generator
# expression stops multi-config generators (Visual Studio) from appending a per-config
# subdirectory. Set before add_subdirectory(vendor) so it also covers libnexus's nxs.
set(DDB_RUNTIME_DIR "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "$<1:${DDB_RUNTIME_DIR}>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "$<1:${DDB_RUNTIME_DIR}>")

# Use vcpkg manifest mode

# Determine platform-specific triplet if not already set
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    if(WIN32)
        set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg triplet")
    elseif(APPLE)
        set(VCPKG_TARGET_TRIPLET "x64-osx" CACHE STRING "vcpkg triplet")
    elseif(UNIX)
        set(VCPKG_TARGET_TRIPLET "x64-linux" CACHE STRING "vcpkg triplet")
    else()
        message(WARNING "Unknown platform, using x64-windows as default triplet")
        set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg triplet")
    endif()
endif()
message(STATUS "Using vcpkg triplet: ${VCPKG_TARGET_TRIPLET}")

# On Windows, define _WINSOCKAPI_ to avoid including winsock.h in windows.h
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Consider position-independent code for all targets (especially for shared libs)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Read version (package.json is the one source: the library, the C ABI and the release share it)
include("${CMAKE_SOURCE_DIR}/cmake/DdbVersion.cmake")
ddb_read_version("${CMAKE_SOURCE_DIR}/package.json")
# A new version re-runs CMake: the version is read here, at configure time
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/package.json")

# Read git commit
set(GIT_REV "")
execute_process(COMMAND git rev-parse --short HEAD
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                OUTPUT_VARIABLE GIT_REV
                ERROR_QUIET)
string(REGEX REPLACE "\n$" "" GIT_REV "${GIT_REV}")
if (NOT "${GIT_REV}" STREQUAL "")
    set(DAPP_VERSION "${APP_VERSION} (git commit ${GIT_REV})")
    set(DAPP_REVISION "${GIT_REV}")
else()
    set(DAPP_VERSION "${APP_VERSION}")
    set(DAPP_REVISION "dev")
endif()

message("DDB Version: ${DAPP_VERSION}")

# The version lives in one generated TU (ddb::version()/ddb::revision(), version.h) instead of
# global -D defines, so a new commit recompiles that file only. configure_file rewrites it only
# when the content changes; like the old defines, the hash is read at configure time.
set(DDB_VERSION_SOURCE "${CMAKE_BINARY_DIR}/generated/version.cpp")
configure_file("${CMAKE_SOURCE_DIR}/src/library/version.cpp.in" "${DDB_VERSION_SOURCE}" @ONLY)

# The C ABI follows the version (ddb_version.h, included by ddb.h). The header is generated into
# its own include folder so it can be installed next to ddb.h; an installed ddb.h never refers
# to the build tree.
set(DDB_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/include")
ddb_generate_version_header("${CMAKE_SOURCE_DIR}/package.json"
    "${CMAKE_SOURCE_DIR}/src/library/ddb_version.h.in" "${DDB_GENERATED_INCLUDE_DIR}/ddb_version.h")

# Find PROJ data folder (vcpkg: share/proj or share/proj4)
find_path(PROJ_DATA_DIR
  NAMES proj.db
  HINTS
    "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/proj"
    "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/proj4"
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/proj"
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/proj4"
  PATH_SUFFIXES share/proj share/proj4
)

if(PROJ_DATA_DIR)
    message(STATUS "Found PROJ data directory: ${PROJ_DATA_DIR}")
else()
    message(WARNING "PROJ data directory not found - proj.db may not be available")
endif()

# Find GDAL data folder (vcpkg: share/gdal). libddb finds it next to itself at run time
# (setupGdalData, process_init.cpp), so it is staged in the runtime folder and packaged.
find_path(GDAL_DATA_DIR
  NAMES gdalvrt.xsd
  HINTS
    "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/gdal"
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/gdal"
  PATH_SUFFIXES share/gdal
)

if(GDAL_DATA_DIR)
    message(STATUS "Found GDAL data directory: ${GDAL_DATA_DIR}")
else()
    message(WARNING "GDAL data directory not found - GDAL will warn about its missing data files")
endif()

# Add custom CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(MsvcReleaseDebugInfo)
include(Warnings)
# DDB_ENABLE_ASAN/UBSAN/TSAN/COVERAGE. Directory-wide options: must stay before
# add_subdirectory(vendor) so the vendored code built in this tree is instrumented too.
include(Sanitizers)

# compile_commands.json for clang-tidy and editors; ignored by the Visual Studio generators
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Option to build tests
option(BUILD_TESTING "Build unit tests" ON)
include(CTest)

set(SRC_LIST "")

# Add vendor directory
add_subdirectory(vendor)

# Add the source code (builds both shared and static libs)
add_subdirectory(src)

# Add applications (main executable)
add_subdirectory(apps)

# Add tests
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation rules
install(TARGETS ${PROJECT_NAME} ${PROJECT_NAME}cmd
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# Install the public headers: the C API and nothing else. libddb exports only the DDB*
# functions of ddb.h, so the C++ headers of src/include could not be used with it. ddb.h includes
# ddb_export.h (the export macro) and ddb_version.h (generated from package.json); the symbol
# manifest lists the exports. Each header compiles on its own (tests/CMakeLists.txt,
# ddb_header_check).
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/include/ddb.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/include/ddb_export.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/include/ddb.symbols"
    "${DDB_GENERATED_INCLUDE_DIR}/ddb_version.h"
    DESTINATION include
)

# Packaging configuration

include(CPack)
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_VERSION ${APP_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "DroneDB client")
set(CPACK_PACKAGE_MAINTAINER "Luca Di Leo <ldileo@digipa.it>")
set(CPACK_PACKAGE_VENDOR "Digipa S.R.L.")
set(CPACK_PACKAGE_CONTACT "support@dronedb.app")
set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${APP_VERSION}-${CMAKE_SYSTEM_NAME}")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libstdc++6, libc6")

# Final message
message(STATUS "CMake setup complete")
