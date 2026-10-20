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
    cctz::cctz
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


target_link_libraries(${PROJECT_NAME} PRIVATE ${COMMON_LIBS})

target_include_directories(${PROJECT_NAME} PRIVATE ${GEOTIFF_INCLUDE_DIR})
target_include_directories(${PROJECT_NAME} PRIVATE ${Stb_INCLUDE_DIR})

# Public headers: consumers (ddbcmd, ddbtest) get these by linking ddb. vendor/libnexus is
# public because 3d.h includes <nxs.h> (installed to include/ by libnexus).
target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/vendor/libnexus>
    $<INSTALL_INTERFACE:include>
)

# Library sources only: glob.hpp (dbops.cpp), and <prefix>/include/pdal because PDAL's own
# headers include their siblings without the pdal/ prefix (ColorinterpFilter.hpp includes
# <filters/StatsFilter.hpp>).
find_path(PDAL_HEADERS_DIR "filters/StatsFilter.hpp" PATH_SUFFIXES pdal REQUIRED)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${PDAL_HEADERS_DIR}
    ${CMAKE_SOURCE_DIR}/vendor/glob
)

# boolinq is header-only and ships no CMake config: an INTERFACE target carries its include
# dir to libddb (registry.cpp) and the tests (utils.cpp).
add_library(boolinq INTERFACE)
target_include_directories(boolinq INTERFACE ${BOOLINQ_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE boolinq)

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLOG_EXPORT)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLOG_GLOBAL)
endif()


if (WIN32)
    set(OUTPUT_DIR "${CMAKE_BINARY_DIR}")
    set(SRC_RELEASE_DIR "${CMAKE_BINARY_DIR}/src/$<CONFIG>")
else()
    set(OUTPUT_DIR "${CMAKE_BINARY_DIR}")
    set(SRC_RELEASE_DIR "${CMAKE_BINARY_DIR}/src")
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

if (WIN32)
    set(PROJ_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/proj")
else()
    set(PROJ_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/proj4")
    if(NOT EXISTS "${PROJ_DIR}/proj.db")
        set(PROJ_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/proj")
    endif()
endif()

if (WIN32)
    set(LIBNEXUS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/vendor/libnexus/$<CONFIG>")
else()
    set(LIBNEXUS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/vendor/libnexus")
endif()

# Copy the necessary files to the binary directory after build
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Copying necessary files to the binary directory..."
    # TARGET_FILE_DIR resolves to the appropriate output directory:
    # - config-specific directory for multi-config generators (e.g. VS)
    # - single output directory for single-config generators (e.g. Ninja)
    # The copy_directory command is executed for all generator types.
    COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>" "${OUTPUT_DIR}"
    # Copy libnexus dll from its output directory
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE_DIR:nxs>/$<TARGET_FILE_NAME:nxs>" "${OUTPUT_DIR}/"
)

# Copy Qt plugins (imageformats, platforms, etc.) for libnexus - required for WebP support on Windows
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_FILE_DIR:nxs>/plugins" "${OUTPUT_DIR}/plugins"
    )
endif()

# Copy PROJ data if available
if(PROJ_DATA_DIR AND EXISTS "${PROJ_DATA_DIR}/proj.db")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Copying PROJ data from ${PROJ_DATA_DIR}..."
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJ_DATA_DIR}/proj.db" "${OUTPUT_DIR}/proj.db"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJ_DATA_DIR}/proj.db" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/proj.db"
        COMMENT "Copying PROJ database"
    )
endif()

# Copy other support files.
# Files are placed in both OUTPUT_DIR (next to the executables) and
# $<TARGET_FILE_DIR:...> (next to the shared library itself) so that the
# runtime data-path lookup in getDataPath() succeeds regardless of which
# directory the DLL is loaded from (e.g. when .NET tests load ddb.dll
# directly from build/src/ rather than from build/).
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/sensor_data.sqlite" "${OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/timezone21.bin" "${OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/sensor-profiles.json" "${OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/zoneinfo" "${OUTPUT_DIR}/zoneinfo"
    # Also copy next to the shared library itself
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/sensor_data.sqlite" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/timezone21.bin" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/sensor-profiles.json" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/zoneinfo" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/zoneinfo"
)

# Windows-specific files
if (WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/ddb.bat" "${OUTPUT_DIR}"
    )

    # Automatic copy of ALL required DLLs next to the executable (MSVC only).
    # Avoids endless manual chasing after gdal/proj/curl/zlib/etc.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E echo "Staging runtime DLLs for ${PROJECT_NAME}..."
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>
                $<TARGET_FILE_DIR:${PROJECT_NAME}>
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
            -DDEST_DIR=${OUTPUT_DIR}
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
