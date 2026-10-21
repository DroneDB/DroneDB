# Gather source files from subdirectories into SRC_LIST
add_subdirectory(library)

message(STATUS "SRC_LIST: ${SRC_LIST}")

# libddb is assembled from two object libraries (DQ-03), each compiled once:
#   ddb_core  every library source except the C API, plus the generated version TU (and the
#             process initialization, ddb::registerProcess in process_init.cpp)
#   ddb_capi  the C API: ddb.cpp, the only TU that implements the extern "C" functions of ddb.h
# libddb (SHARED) = ddb_core + ddb_capi. ddb_unit_tests (tests/CMakeLists.txt) links the same
# objects statically and never libddb: no binary may contain both, otherwise the plog instance
# (PLOG_GLOBAL) and the library's singletons would exist twice in one process.
set(DDB_CAPI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/library/ddb.cpp")
set(DDB_CORE_SOURCES ${SRC_LIST})
list(REMOVE_ITEM DDB_CORE_SOURCES ${DDB_CAPI_SOURCES})

# Find and link dependencies from vcpkg
find_package(Threads REQUIRED)
find_package(CCTZ CONFIG REQUIRED)
find_package(exiv2 CONFIG REQUIRED)
find_package(GDAL CONFIG REQUIRED)
find_package(PDAL CONFIG REQUIRED)
find_package(PROJ CONFIG REQUIRED)
find_package(GeoTIFF CONFIG REQUIRED)
find_package(LibZip CONFIG REQUIRED)
find_package(plog CONFIG REQUIRED)
find_package(cpr CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(SpatiaLite REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)
find_package(draco CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

find_path(BOOLINQ_INCLUDE_DIRS "boolinq/boolinq.h" REQUIRED)

# Common libraries
set(COMMON_LIBS
    pdalcpp
    Threads::Threads
    Exiv2::exiv2lib
    GDAL::GDAL
    PROJ::proj
    ${GEOTIFF_LIBRARIES}
    libzip::zip
    plog::plog
    cpr::cpr
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    unofficial::sqlite3::sqlite3
    spatialite::spatialite
    nxs
    assimp::assimp
    KTX::ktx
    tinyobjloader::tinyobjloader
    draco::draco
    spz
    lazperf
    ZLIB::ZLIB
)


# boolinq is header-only and ships no CMake config: an INTERFACE target carries its include
# dir to libddb (registry.cpp) and the tests (utils.cpp).
add_library(boolinq INTERFACE)
target_include_directories(boolinq SYSTEM INTERFACE ${BOOLINQ_INCLUDE_DIRS})

# Usage requirements, defined once (DQ-03):
#   ddb_headers     what code including the public headers needs: the include directories,
#                   cctz (timezone.h exposes cctz::time_zone) and the plog mode, which must be
#                   the same in libddb and in every module that logs through it. PUBLIC on ddb,
#                   so ddbcmd and ddbtest get it by linking ddb.
#   ddb_core_usage  ddb_headers plus every library dependency (COMMON_LIBS): what the library
#                   objects compile with and what ddb_unit_tests, which contains them, links.
#                   ddb links COMMON_LIBS itself (see the note on link order below).
add_library(ddb_headers INTERFACE)
# vendor/libnexus is public because 3d.h includes <nxs.h> (installed to include/ by libnexus)
target_include_directories(ddb_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(ddb_headers SYSTEM INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/vendor/libnexus>
)
target_link_libraries(ddb_headers INTERFACE cctz::cctz)
if(WIN32)
    target_compile_definitions(ddb_headers INTERFACE PLOG_EXPORT)
else()
    target_compile_definitions(ddb_headers INTERFACE PLOG_GLOBAL)
endif()

# nxs is link-only here: Qt5::Gui, reached through nxs, adds its static-plugin import sources
# (INTERFACE_SOURCES) to every target that links it, and the object libraries would each compile
# their own copy into libddb
list(TRANSFORM COMMON_LIBS REPLACE "^nxs$" "$<LINK_ONLY:nxs>" OUTPUT_VARIABLE DDB_CORE_LIBS)
add_library(ddb_core_usage INTERFACE)
target_link_libraries(ddb_core_usage INTERFACE ${DDB_CORE_LIBS} boolinq ddb_headers)

# Library sources only: glob.hpp (dbops.cpp), and <prefix>/include/pdal because PDAL's own
# headers include their siblings without the pdal/ prefix (ColorinterpFilter.hpp includes
# <filters/StatsFilter.hpp>).
find_path(PDAL_HEADERS_DIR "filters/StatsFilter.hpp" PATH_SUFFIXES pdal REQUIRED)

# ddb_add_library_objects(<name> <sources>...): an object library of libddb sources with the
# compile settings that apply to library code only: the warnings (counted by the ratchet,
# scripts/check-warnings.py), the export macro, the MSVC Release debug info and the private
# third-party include directories (SYSTEM: their warnings are not ours, cmake/Warnings.cmake).
function(ddb_add_library_objects name)
    add_library(${name} OBJECT ${ARGN})
    ddb_msvc_release_debug_info(${name})
    target_link_libraries(${name} PUBLIC ddb_core_usage PRIVATE ddb_warnings)
    target_compile_definitions(${name} PRIVATE MODERN_CPP_LIB_EXPORTS)
    target_include_directories(${name} SYSTEM PRIVATE
        ${GEOTIFF_INCLUDE_DIR}
        ${Stb_INCLUDE_DIR}
        ${PDAL_HEADERS_DIR}
        ${CMAKE_SOURCE_DIR}/vendor/glob
    )
    add_dependencies(${name} nxs)
endfunction()

ddb_add_library_objects(ddb_core ${DDB_CORE_SOURCES} ${DDB_VERSION_SOURCE})
ddb_add_library_objects(ddb_capi ${DDB_CAPI_SOURCES})

# The symbol manifest (XI-01): include/ddb.symbols lists every exported DDB* function, one per
# line ('#' starts a comment line). It is the only copy of the list: DDBGetCapabilities() (ddb.cpp)
# includes it as generated/ddb_symbols.inc, one string literal per name, and scripts/check-abi.py
# compares it with the symbols libddb exports. A change to the manifest re-runs CMake;
# file(CONFIGURE) rewrites the include only when its content changes.
set(DDB_SYMBOLS_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/include/ddb.symbols")
set(DDB_SYMBOLS_MANIFEST "${DDB_SYMBOLS_MANIFEST}" PARENT_SCOPE)  # tests/CMakeLists.txt reads it too
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DDB_SYMBOLS_MANIFEST}")
# Every line that is not empty or a comment (a comment may hold ';', a list separator)
file(STRINGS "${DDB_SYMBOLS_MANIFEST}" _ddb_symbols REGEX "^[^#]")
set(_ddb_symbols_inc "")
foreach(_symbol IN LISTS _ddb_symbols)
    if(NOT _symbol MATCHES "^DDB[A-Za-z0-9_]+$")
        message(FATAL_ERROR "${DDB_SYMBOLS_MANIFEST}: '${_symbol}' is not a DDB* symbol name")
    endif()
    string(APPEND _ddb_symbols_inc "\"${_symbol}\",\n")
endforeach()
if(_ddb_symbols_inc STREQUAL "")
    message(FATAL_ERROR "${DDB_SYMBOLS_MANIFEST} lists no symbol")
endif()
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/generated/ddb_symbols.inc" CONTENT "${_ddb_symbols_inc}")
target_include_directories(ddb_capi PRIVATE "${CMAKE_BINARY_DIR}/generated")

add_library(${PROJECT_NAME} SHARED $<TARGET_OBJECTS:ddb_core> $<TARGET_OBJECTS:ddb_capi>)
ddb_msvc_release_debug_info(${PROJECT_NAME})

# On Windows, export every symbol of libddb automatically. The .def file is generated from all
# the objects linked into ddb.dll, including the TARGET_OBJECTS of ddb_core and ddb_capi.
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# COMMON_LIBS directly, in this order, rather than through ddb_core_usage: the order decides which
# static archive members end up in libddb.so (and so its exported symbols), and an INTERFACE
# target in between reorders the link line
target_link_libraries(${PROJECT_NAME} PRIVATE ${COMMON_LIBS} boolinq PUBLIC ddb_headers)

find_package(Python3 REQUIRED)

# Define a custom target to update sensor data using the Python script
add_custom_target(
    update_sensor_data ALL
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/data/create_sensor_data.py"
        "${CMAKE_SOURCE_DIR}/data/ddb_sensor_data.json"
        "${CMAKE_BINARY_DIR}/sensor_data.sqlite"
    COMMENT "Updating sensor data SQLite database..."
)

message(STATUS "Build target 'update_sensor_data' is ready.")

# Runtime files next to libddb: ddb, nxs, ddbcmd and the test binaries are all built in
# DDB_RUNTIME_DIR (root CMakeLists.txt), so getDataPath() finds these files whether it looks next
# to the library or next to the executable. On Windows, libnexus's own post-build step copies
# the Qt plugins (WebP textures) next to nxs.dll, i.e. to DDB_RUNTIME_DIR/plugins.

# ddb_stage_runtime_dlls(<target>): on Windows, copy the DLLs <target> links to DDB_RUNTIME_DIR
# after it is built (TARGET_RUNTIME_DLLS: direct dependencies only).
function(ddb_stage_runtime_dlls target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E echo "Staging runtime DLLs for ${target}..."
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${target}>
                    "${DDB_RUNTIME_DIR}"
            COMMAND_EXPAND_LISTS
        )
    endif()
endfunction()

# ddb_stage_runtime_files(<target>): everything a binary that contains the library code needs
# at run time, copied to DDB_RUNTIME_DIR after <target> is built: the data files getDataPath()
# looks up (PROJ database, sensor data, timezone data) and, on Windows, all the DLLs, so ddb and
# ddb_unit_tests (which does not depend on ddb) each run on their own.
function(ddb_stage_runtime_files target)
    if(PROJ_DATA_DIR AND EXISTS "${PROJ_DATA_DIR}/proj.db")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJ_DATA_DIR}/proj.db" "${DDB_RUNTIME_DIR}"
            COMMENT "Copying PROJ database from ${PROJ_DATA_DIR}"
        )
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/data/sensor_data.sqlite"
            "${CMAKE_SOURCE_DIR}/data/timezone21.bin"
            "${CMAKE_SOURCE_DIR}/data/sensor-profiles.json"
            "${DDB_RUNTIME_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/zoneinfo" "${DDB_RUNTIME_DIR}/zoneinfo"
    )

    if(WIN32)
        # Automatic copy of ALL required DLLs next to the library and executables (MSVC only).
        # Avoids endless manual chasing after gdal/proj/curl/zlib/etc.
        ddb_stage_runtime_dlls(${target})

        # Copy ALL DLLs from vcpkg bin directories to ensure transitive dependencies are included
        # TARGET_RUNTIME_DLLS only copies direct dependencies, not transitive ones like Qt5's harfbuzz, pcre2, etc.
        set(VCPKG_BIN_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin")
        set(VCPKG_DEBUG_BIN_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/debug/bin")

        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E echo "Copying vcpkg DLLs to output directory..."
            COMMAND "${CMAKE_COMMAND}"
                -DSOURCE_DIR=$<IF:$<CONFIG:Debug>,${VCPKG_DEBUG_BIN_DIR},${VCPKG_BIN_DIR}>
                $<$<CONFIG:Debug>:-DFALLBACK_DIR=${VCPKG_BIN_DIR}>
                -DDEST_DIR=${DDB_RUNTIME_DIR}
                -P "${CMAKE_SOURCE_DIR}/cmake/CopyDlls.cmake"
            COMMENT "Copying vcpkg DLLs"
        )
    endif()
endfunction()

ddb_stage_runtime_files(${PROJECT_NAME})

# Windows-specific files
if (WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/ddb.bat" "${DDB_RUNTIME_DIR}"
    )
endif()


# CA bundle for libcurl (remote reads in thumbs.cpp and pctiler.cpp, found via getDataPath):
# Mozilla's roots as published by curl.se, pinned by date and SHA-256 so every build ships the
# same file. It is fetched at configure time, only when the file is missing or differs.
# To update: take the newest dated file and its SHA-256 from https://curl.se/docs/caextract.html
set(DDB_CA_BUNDLE_DATE "2026-10-20")
set(DDB_CA_BUNDLE_SHA256 "796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe")
set(CURL_CA_BUNDLE "${CMAKE_BINARY_DIR}/curl-ca-bundle.crt")
set(_ca_bundle_hash "")
if(EXISTS "${CURL_CA_BUNDLE}")
    file(SHA256 "${CURL_CA_BUNDLE}" _ca_bundle_hash)
endif()
if(NOT _ca_bundle_hash STREQUAL DDB_CA_BUNDLE_SHA256)
    set(_ca_bundle_url "https://curl.se/ca/cacert-${DDB_CA_BUNDLE_DATE}.pem")
    message(STATUS "Downloading CA bundle ${_ca_bundle_url}")
    # Download next to the target and rename, so a failed download never leaves a bad bundle
    file(DOWNLOAD "${_ca_bundle_url}" "${CURL_CA_BUNDLE}.download"
        EXPECTED_HASH SHA256=${DDB_CA_BUNDLE_SHA256}
        TLS_VERIFY ON
        STATUS _ca_bundle_status)
    list(GET _ca_bundle_status 0 _ca_bundle_code)
    if(NOT _ca_bundle_code EQUAL 0)
        file(REMOVE "${CURL_CA_BUNDLE}.download")
        message(FATAL_ERROR "CA bundle download failed: ${_ca_bundle_status}")
    endif()
    file(RENAME "${CURL_CA_BUNDLE}.download" "${CURL_CA_BUNDLE}")
endif()

# set(OUTPUT_DIST_FILE "${PROJECT_NAME}-${APP_VERSION}-${PLATFORM_NAME}.${OUTPUT_DIST_EXT}")
