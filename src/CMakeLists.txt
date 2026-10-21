# Gather source files from subdirectories into SRC_LIST
add_subdirectory(library)

message(STATUS "SRC_LIST: ${SRC_LIST}")

add_library(${PROJECT_NAME} SHARED ${SRC_LIST} ${DDB_VERSION_SOURCE})
ddb_msvc_release_debug_info(${PROJECT_NAME})

# On Windows, ensure all symbols are exported automatically from the shared lib
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# Define export macro for cross-platform symbol exports in shared library
target_compile_definitions(${PROJECT_NAME} PRIVATE MODERN_CPP_LIB_EXPORTS)

add_dependencies(${PROJECT_NAME} nxs)

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


target_link_libraries(${PROJECT_NAME} PRIVATE ${COMMON_LIBS} ddb_warnings)
# timezone.h exposes cctz::time_zone in the public API, so consumers (ddbtest) link cctz too
target_link_libraries(${PROJECT_NAME} PUBLIC cctz::cctz)

# Third-party include directories are SYSTEM: their warnings are not ours (cmake/Warnings.cmake)
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${GEOTIFF_INCLUDE_DIR} ${Stb_INCLUDE_DIR})

# Public headers: consumers (ddbcmd, ddbtest) get these by linking ddb. vendor/libnexus is
# public because 3d.h includes <nxs.h> (installed to include/ by libnexus).
target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(${PROJECT_NAME} SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/vendor/libnexus>
)

# Library sources only: glob.hpp (dbops.cpp), and <prefix>/include/pdal because PDAL's own
# headers include their siblings without the pdal/ prefix (ColorinterpFilter.hpp includes
# <filters/StatsFilter.hpp>).
find_path(PDAL_HEADERS_DIR "filters/StatsFilter.hpp" PATH_SUFFIXES pdal REQUIRED)
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
    ${PDAL_HEADERS_DIR}
    ${CMAKE_SOURCE_DIR}/vendor/glob
)

# boolinq is header-only and ships no CMake config: an INTERFACE target carries its include
# dir to libddb (registry.cpp) and the tests (utils.cpp).
add_library(boolinq INTERFACE)
target_include_directories(boolinq SYSTEM INTERFACE ${BOOLINQ_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE boolinq)

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLOG_EXPORT)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLOG_GLOBAL)
endif()


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
if(PROJ_DATA_DIR AND EXISTS "${PROJ_DATA_DIR}/proj.db")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJ_DATA_DIR}/proj.db" "${DDB_RUNTIME_DIR}"
        COMMENT "Copying PROJ database from ${PROJ_DATA_DIR}"
    )
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/data/sensor_data.sqlite"
        "${CMAKE_SOURCE_DIR}/data/timezone21.bin"
        "${CMAKE_SOURCE_DIR}/data/sensor-profiles.json"
        "${DDB_RUNTIME_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/zoneinfo" "${DDB_RUNTIME_DIR}/zoneinfo"
)

# Windows-specific files
if (WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/ddb.bat" "${DDB_RUNTIME_DIR}"
    )

    # Automatic copy of ALL required DLLs next to the library and executables (MSVC only).
    # Avoids endless manual chasing after gdal/proj/curl/zlib/etc.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E echo "Staging runtime DLLs for ${PROJECT_NAME}..."
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>
                "${DDB_RUNTIME_DIR}"
        COMMAND_EXPAND_LISTS
    )

    # Copy ALL DLLs from vcpkg bin directories to ensure transitive dependencies are included
    # TARGET_RUNTIME_DLLS only copies direct dependencies, not transitive ones like Qt5's harfbuzz, pcre2, etc.
    set(VCPKG_BIN_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin")
    set(VCPKG_DEBUG_BIN_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/debug/bin")

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E echo "Copying vcpkg DLLs to output directory..."
        COMMAND "${CMAKE_COMMAND}"
            -DSOURCE_DIR=$<IF:$<CONFIG:Debug>,${VCPKG_DEBUG_BIN_DIR},${VCPKG_BIN_DIR}>
            $<$<CONFIG:Debug>:-DFALLBACK_DIR=${VCPKG_BIN_DIR}>
            -DDEST_DIR=${DDB_RUNTIME_DIR}
            -P "${CMAKE_SOURCE_DIR}/cmake/CopyDlls.cmake"
        COMMENT "Copying vcpkg DLLs"
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
