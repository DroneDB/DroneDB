# Gather source files from subdirectories into SRC_LIST
#add_subdirectory(utilities)
add_subdirectory(library)

message(STATUS "SRC_LIST: ${SRC_LIST}")

add_library(${PROJECT_NAME} SHARED ${SRC_LIST})

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
find_package(GeoTIFF CONFIG REQUIRED)
find_package(LibZip CONFIG REQUIRED)
find_package(plog CONFIG REQUIRED)
find_package(cpr CONFIG REQUIRED)
find_package(cxxopts CONFIG REQUIRED)
find_package(unofficial-hash-library CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(SpatiaLite REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

find_path(BOOLINQ_INCLUDE_DIRS "boolinq/boolinq.h")

# Common libraries
set(COMMON_LIBS
    Threads::Threads
    cctz::cctz
    Exiv2::exiv2lib
    GDAL::GDAL
    $<TARGET_NAME:pdalcpp>
    libzip::zip
    plog::plog
    cpr::cpr
    nlohmann_json::nlohmann_json
    cxxopts::cxxopts
    unofficial::hash-library
    unofficial::sqlite3::sqlite3
    spatialite::spatialite
    nxs
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${COMMON_LIBS})

# Include directories (public for external use)
target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Include paths
if (MINGW)
    set(UNTWINE_OS_DIR untwine/mingw)
elseif (WIN32)
    set(UNTWINE_OS_DIR untwine/windows)
elseif (APPLE)
    set(UNTWINE_OS_DIR untwine/osx)
elseif (UNIX)
    set(UNTWINE_OS_DIR untwine/generic)
else()
    message(FATAL_ERROR "OS not supported")
endif()

find_path(BOOLINQ_INCLUDE_DIRS "boolinq/boolinq.h")

set (CUSTOM_INCLUDE_DIRS
        ${BOOLINQ_INCLUDE_DIRS}
        ${BOOLINQ_INCLUDE_DIRS}/pdal
        ${CMAKE_SOURCE_DIR}/vendor/untwine
        ${CMAKE_SOURCE_DIR}/vendor/untwine/${UNTWINE_OS_DIR}
        ${CMAKE_SOURCE_DIR}/vendor/libnexus
        )

# Propagate the variable to the parent scope
set(CUSTOM_INCLUDE_DIRS ${CUSTOM_INCLUDE_DIRS} PARENT_SCOPE)

message(STATUS "lib CUSTOM_INCLUDE_DIRS: ${CUSTOM_INCLUDE_DIRS}")

target_include_directories(${PROJECT_NAME} PUBLIC ${CUSTOM_INCLUDE_DIRS})

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLOG_EXPORT)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLOG_GLOBAL)
endif()


set(CURL_CA_BUNDLE ${CMAKE_BINARY_DIR}/curl-ca-bundle.crt)

if (WIN32)
    set(SYSTEM_TEMP_DIR $ENV{TEMP})
    set(OUTPUT_DIR "${CMAKE_BINARY_DIR}")
    set(SRC_RELEASE_DIR "${CMAKE_BINARY_DIR}/src/$<CONFIG>")
else()
    set(SYSTEM_TEMP_DIR /tmp)
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
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${SRC_RELEASE_DIR}" "${OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJ_DIR}/proj.db" "${OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/sensor_data.sqlite" "${OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/timezone21.bin" "${OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/zoneinfo" "${OUTPUT_DIR}/zoneinfo"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${LIBNEXUS_OUTPUT_DIR}" "${OUTPUT_DIR}"
)

if (WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/ddb.bat" "${OUTPUT_DIR}"
    )
endif()


# Create a unique subdirectory in the system temp directory for our files
set(TEMP_DIR ${SYSTEM_TEMP_DIR}/cmake_curl_temp_${PROJECT_NAME})
file(MAKE_DIRECTORY ${TEMP_DIR})

# Add custom command to download the file using curl in the temporary directory
add_custom_command(
    OUTPUT ${TEMP_DIR}/cacert.pem
    COMMAND curl --etag-compare ${TEMP_DIR}/etag.txt --etag-save ${TEMP_DIR}/etag.txt --remote-name https://curl.se/ca/cacert.pem
    COMMENT "Downloading updated CA certificate if modified"
    WORKING_DIRECTORY ${TEMP_DIR}
)

# Copy the downloaded file to the binary directory with the desired name
add_custom_command(
    OUTPUT ${CURL_CA_BUNDLE}
    COMMAND ${CMAKE_COMMAND} -E copy ${TEMP_DIR}/cacert.pem ${CURL_CA_BUNDLE}
    DEPENDS ${TEMP_DIR}/cacert.pem
    COMMENT "Copying CA certificate to ${CURL_CA_BUNDLE}"
)

add_custom_target(
    download_ca_bundle ALL
    DEPENDS ${CURL_CA_BUNDLE}
)

# Make sure the library target depends on the CA bundle
add_dependencies(${PROJECT_NAME} download_ca_bundle)

add_custom_target(
    clean_temp
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${TEMP_DIR}
    COMMENT "Cleaning up temporary directory in system temp folder"
)

message(STATUS "Build target 'download_ca_bundle' is ready. Temporary files are stored in ${TEMP_DIR}.")

# set(OUTPUT_DIST_FILE "${PROJECT_NAME}-${APP_VERSION}-${PLATFORM_NAME}.${OUTPUT_DIST_EXT}")
