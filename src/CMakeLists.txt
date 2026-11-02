# Gather source files from subdirectories into SRC_LIST
add_subdirectory(library)

message(STATUS "SRC_LIST: ${SRC_LIST}")

# libddb is assembled from two object libraries, each compiled once:
#   ddb_core  every library source except the C API, plus the generated version TU (and the
#             process initialization, ddb::registerProcess in process_init.cpp)
#   ddb_capi  the C API: ddb.cpp, the only TU that implements the extern "C" functions of ddb.h,
#             and c_api_util.cpp, its error mapper and argument helpers (c_api_util.h, private),
#             and c_api_capabilities.cpp, the JSON of DDBGetCapabilities
# libddb (SHARED) = ddb_core + ddb_capi. ddbcmd, ddbtest and ddb_unit_tests contain the same
# objects (ddb_embedded below) and never load libddb: no binary may contain both, otherwise the
# plog instance and the library's singletons would exist twice in one process.
#
# libddb exports the C API and nothing else. Every object is compiled with hidden
# visibility; only the DDB* functions of ddb.h are marked DDB_DLL (ddb_export.h), and
# on Linux a version script made from the symbol manifest hides every other symbol, the C++
# symbols of the static libraries linked into libddb included. The C++ code is reached by
# linking the objects (ddb_embedded), not through libddb.
set(DDB_CAPI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/library/ddb.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/library/c_api_util.cpp"
                     "${CMAKE_CURRENT_SOURCE_DIR}/library/c_api_capabilities.cpp")
set(DDB_CORE_SOURCES ${SRC_LIST})
list(REMOVE_ITEM DDB_CORE_SOURCES ${DDB_CAPI_SOURCES})

# Find and link dependencies from vcpkg
find_package(Threads REQUIRED)
find_package(CCTZ CONFIG REQUIRED)
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
    assimp::assimp
    KTX::ktx
    tinyobjloader::tinyobjloader
    draco::draco
    spz
    lazperf
    ZLIB::ZLIB
)


# boolinq is header-only and ships no CMake config: an INTERFACE target carries its include
# dir to libddb (pull.cpp) and the tests (utils.cpp).
add_library(boolinq INTERFACE)
target_include_directories(boolinq SYSTEM INTERFACE ${BOOLINQ_INCLUDE_DIRS})

# Usage requirements, defined once:
#   ddb_c_api       what code that uses libddb as a shared library needs: the folder of ddb.h and
#                   ddb_export.h, and of the generated ddb_version.h. PUBLIC on ddb. On Windows
#                   ddb_export.h makes the DDB* declarations dllimport.
#   ddb_headers     ddb_c_api plus what the C++ headers of the library need: the include
#                   directories, cctz (timezone.h exposes cctz::time_zone) and the plog mode,
#                   the same in every module that logs. What the library objects compile with.
#   ddb_static_api  ddb_headers for code that is linked with the library objects, not with
#                   libddb: DDB_STATIC_DEFINE makes ddb_export.h declare the DDB* functions
#                   without dllimport (the CLI commands, the test support and the C TU).
#   ddb_core_usage  ddb_headers plus every library dependency (COMMON_LIBS): what the library
#                   objects compile with and what a binary that contains them links.
#                   ddb links COMMON_LIBS itself (see the note on link order below).
#   ddb_embed_library()  (below) makes a binary contain the library: ddbcmd, ddbtest and
#                   ddb_unit_tests.
add_library(ddb_c_api INTERFACE)
target_include_directories(ddb_c_api INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${DDB_GENERATED_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:include>
)
add_library(ddb_headers INTERFACE)
target_link_libraries(ddb_headers INTERFACE ddb_c_api)
add_library(ddb_static_api INTERFACE)
target_link_libraries(ddb_static_api INTERFACE ddb_headers)
target_compile_definitions(ddb_static_api INTERFACE DDB_STATIC_DEFINE)
target_link_libraries(ddb_headers INTERFACE cctz::cctz)
# plog's char is UTF-8 everywhere (mio.h, "Path encoding"). Without it MSVC builds read every
# narrow log string in the ANSI code page. One value for every target: plog's types depend on
# it.
target_compile_definitions(ddb_headers INTERFACE PLOG_CHAR_IS_UTF8=1)
# The logger instance is private to each binary (libddb, ddbcmd, ddbtest, ddb_unit_tests): no two
# modules of a process share it, and libddb exports nothing but its C API. plog exported
# its instance through PLOG_GLOBAL (default visibility) and, on Windows, PLOG_EXPORT.
if(NOT WIN32)
    target_compile_definitions(ddb_headers INTERFACE PLOG_LOCAL)
endif()

add_library(ddb_core_usage INTERFACE)
target_link_libraries(ddb_core_usage INTERFACE ${COMMON_LIBS} boolinq ddb_headers)
# Windows: the folders the host makes for the output of the raster helper get a low integrity label
# (raster_artefacts.cpp), which the helper's server, running at low integrity, needs to write. The
# sandbox library depends on nothing of libddb and holds no GPL code.
if(WIN32)
    target_link_libraries(ddb_core_usage INTERFACE ddb_sandbox)
endif()

# Library sources only: glob.hpp (path_expansion.cpp), and <prefix>/include/pdal because PDAL's own
# headers include their siblings without the pdal/ prefix (ColorinterpFilter.hpp includes
# <filters/StatsFilter.hpp>).
find_path(PDAL_HEADERS_DIR "filters/StatsFilter.hpp" PATH_SUFFIXES pdal REQUIRED)

# ddb_add_library_objects(<name> <sources>...): an object library of libddb sources with the
# compile settings that apply to library code only: the warnings (counted by the ratchet,
# scripts/check-warnings.py), hidden visibility and the export macro (only ddb.h's DDB_DLL
# functions are visible), the MSVC Release debug info and the private third-party
# include directories (SYSTEM: their warnings are not ours, cmake/Warnings.cmake).
function(ddb_add_library_objects name)
    add_library(${name} OBJECT ${ARGN})
    ddb_msvc_release_debug_info(${name})
    target_link_libraries(${name} PUBLIC ddb_core_usage PRIVATE ddb_warnings)
    # DDB_BUILDING_LIBRARY: ddb_export.h makes DDB_DLL an export (it is defined for these objects,
    # not for the ddb target, which has no source of its own)
    # ZD_EXPORT empty: vendor/zonedetect is compiled into libddb, but its header declares the API
    # dllimport on MSVC unless ZD_EXPORT is set, and MSVC exports a definition of a dllimport
    # function (C4273), so ddb.dll exported the 13 ZD* functions
    target_compile_definitions(${name} PRIVATE DDB_BUILDING_LIBRARY ZD_EXPORT=)
    set_target_properties(${name} PROPERTIES
        CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_include_directories(${name} SYSTEM PRIVATE
        ${GEOTIFF_INCLUDE_DIR}
        ${Stb_INCLUDE_DIR}
        ${PDAL_HEADERS_DIR}
        ${CMAKE_SOURCE_DIR}/vendor/glob
    )
    # One section per function and per datum, so that --gc-sections (ddb_gc_sections) can drop what
    # the binary does not reach. MSVC does it by default (/Gy, /OPT:REF in a Release link).
    target_compile_options(${name} PRIVATE
        $<$<AND:$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>,$<NOT:$<PLATFORM_ID:Darwin>>>:-ffunction-sections -fdata-sections>)
endfunction()

# ddb_gc_sections(<target>): the linker drops the sections nothing refers to (the library is built
# with one section per function). Linux/ELF with GNU or Clang only. Stripped, libddb.so is 12 %
# smaller and ddbcmd 5 % (measured).
function(ddb_gc_sections target)
    if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(${target} PRIVATE LINKER:--gc-sections)
    endif()
endfunction()

ddb_add_library_objects(ddb_core ${DDB_CORE_SOURCES} ${DDB_VERSION_SOURCE})
ddb_add_library_objects(ddb_capi ${DDB_CAPI_SOURCES})

# The symbol manifest: include/ddb.symbols lists every exported DDB* function, one per
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
set(_ddb_exported_names "")
foreach(_symbol IN LISTS _ddb_symbols)
    if(NOT _symbol MATCHES "^DDB[A-Za-z0-9_]+$")
        message(FATAL_ERROR "${DDB_SYMBOLS_MANIFEST}: '${_symbol}' is not a DDB* symbol name")
    endif()
    string(APPEND _ddb_symbols_inc "\"${_symbol}\",\n")
    string(APPEND _ddb_exported_names "        ${_symbol};\n")
endforeach()
if(_ddb_symbols_inc STREQUAL "")
    message(FATAL_ERROR "${DDB_SYMBOLS_MANIFEST} lists no symbol")
endif()
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/generated/ddb_symbols.inc" CONTENT "${_ddb_symbols_inc}")
target_include_directories(ddb_capi PRIVATE "${CMAKE_BINARY_DIR}/generated")

add_library(${PROJECT_NAME} SHARED $<TARGET_OBJECTS:ddb_core> $<TARGET_OBJECTS:ddb_capi>)
ddb_msvc_release_debug_info(${PROJECT_NAME})

# DDB_DLL (ddb_export.h), the one export macro, is on the DDB* functions of ddb.h only; the library
# is built with hidden visibility, so nothing else is exported, and on Windows there is no
# WINDOWS_EXPORT_ALL_SYMBOLS: ddb.dll exports what ddb.h declares.
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# Linux: a version script made from the manifest exports exactly the manifest, whatever the
# static libraries linked into libddb (cpr, spz, draco, Assimp, ...) export on their own, and
# also drops the linker's own symbols (_init, _fini, _edata, ...). scripts/check-abi.py exports
# checks the result (nm -D). The visibility of the objects keeps the C++ symbols of the library
# itself out; the script is the guarantee for the rest.
if(UNIX AND NOT APPLE)
    set(DDB_VERSION_SCRIPT "${CMAKE_BINARY_DIR}/generated/ddb.version")
    file(CONFIGURE OUTPUT "${DDB_VERSION_SCRIPT}" @ONLY CONTENT
"{\n    global:\n${_ddb_exported_names}    local:\n        *;\n};\n")
    target_link_options(${PROJECT_NAME} PRIVATE "LINKER:--version-script=${DDB_VERSION_SCRIPT}")
    set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS "${DDB_VERSION_SCRIPT}")
endif()

# COMMON_LIBS directly, in this order, rather than through ddb_core_usage: the order decides which
# static archive members end up in libddb.so, and an INTERFACE target in between reorders the
# link line
target_link_libraries(${PROJECT_NAME} PRIVATE ${COMMON_LIBS} boolinq ddb_headers PUBLIC ddb_c_api)
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ddb_sandbox)
endif()
ddb_gc_sections(${PROJECT_NAME})

# The helper programs (scripts/helper-tools.txt: bm3d, which writes bm3d model files, and
# metadata-util, which reads and writes image metadata) are started by libddb (bm3d_runner.h,
# exif_helper_pool.h), which links none of their code. They are built next to libddb, which is
# where the tool locator looks for them.
add_dependencies(${PROJECT_NAME} ${DDB_HELPER_TOOLS})

# ddb_embed_library(<target>): <target> contains the library instead of loading libddb: the
# objects of ddb_core and ddb_capi, the libraries they need and DDB_STATIC_DEFINE (ddb_static_api).
# This is how the CLI and the test binaries reach the C++ code, which libddb does not export.
function(ddb_embed_library target)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:ddb_core> $<TARGET_OBJECTS:ddb_capi>)
    target_link_libraries(${target} PRIVATE ddb_core_usage ddb_static_api)
    ddb_gc_sections(${target})
endfunction()

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

# Runtime files next to libddb: ddb, the helper programs, ddbcmd and the test binaries are all
# built in DDB_RUNTIME_DIR (root CMakeLists.txt), so getDataPath() finds these files whether it
# looks next to the library or next to the executable.

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
# looks up (PROJ database, GDAL data, sensor data, timezone data) and, on Windows, all the DLLs, so ddb and
# ddb_unit_tests (which does not depend on ddb) each run on their own.
function(ddb_stage_runtime_files target)
    if(PROJ_DATA_DIR AND EXISTS "${PROJ_DATA_DIR}/proj.db")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJ_DATA_DIR}/proj.db" "${DDB_RUNTIME_DIR}"
            COMMENT "Copying PROJ database from ${PROJ_DATA_DIR}"
        )
    endif()

    if(GDAL_DATA_DIR)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DSOURCE_DIR=${GDAL_DATA_DIR}
                -DDEST_DIR=${DDB_RUNTIME_DIR}/gdal
                -P "${CMAKE_SOURCE_DIR}/cmake/StageGdalData.cmake"
            COMMENT "Copying GDAL data from ${GDAL_DATA_DIR}"
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
        # TARGET_RUNTIME_DLLS only copies direct dependencies, not transitive ones like pcre2, etc.
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
