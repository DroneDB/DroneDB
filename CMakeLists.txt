cmake_minimum_required(VERSION 2.8)

cmake_policy(SET CMP0048 NEW)

# Used to show all CMake constants
#set (CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_LIST_DIR}/CompilerOptions.cmake")
project(ddb)

# Read version
file(READ "package.json" PACKAGE_JSON)
string(REGEX MATCH "\"version\": \"[0-9]+\\.[0-9]+\\.[0-9]+" APP_VERSION ${PACKAGE_JSON})
string(SUBSTRING ${APP_VERSION} 12 -1 APP_VERSION)
message("DDB Version: ${APP_VERSION}")
add_compile_options("-DAPP_VERSION=\"${APP_VERSION}\"")

# Disable MS warnings
if (WIN32)
    add_compile_options("-D_CRT_SECURE_NO_WARNINGS=1")
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING
      "Choose the type of build, options are: Debug Release
RelWithDebInfo MinSizeRel."
      FORCE)
endif(NOT CMAKE_BUILD_TYPE)

if (WIN32)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_BUILD_TYPE}")
endif()

find_program(CCACHE_FOUND ccache)
if(CCACHE_FOUND)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
endif(CCACHE_FOUND)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(${CMAKE_ROOT}/Modules/ExternalProject.cmake)
set(DOWNLOADS_DIR "${CMAKE_BINARY_DIR}/downloads")

if (WIN32)
    # Download GDAL/CURL/sqlite3/spatialite libraries
    if (NOT EXISTS "${DOWNLOADS_DIR}/gdal_libs.zip")
        message("Downloading GDAL libs...")
        file(DOWNLOAD "https://github.com/DroneDB/ddb_data/releases/download/v0.9.2/release-1911-x64-gdal-3-0-4-mapserver-7-4-3-libs.zip" "${DOWNLOADS_DIR}/gdal_libs.zip")
    endif()

    if (NOT EXISTS "${DOWNLOADS_DIR}/gdal_bins.zip")
        message("Downloading GDAL binaries...")
        file(DOWNLOAD "https://github.com/DroneDB/ddb_data/releases/download/v0.9.2/release-1911-x64-gdal-3-0-4-mapserver-7-4-3.zip" "${DOWNLOADS_DIR}/gdal_bins.zip")
    endif()

    if (NOT EXISTS "${DOWNLOADS_DIR}/gdal/include")
        message("Extracting GDAL libs...")
        execute_process(COMMAND powershell Expand-Archive "\"${DOWNLOADS_DIR}/gdal_libs.zip\"" "\"${DOWNLOADS_DIR}/gdal\"")
    endif()

    if (NOT EXISTS "${DOWNLOADS_DIR}/gdal/bin")
        message("Extracting GDAL bins...")
        execute_process(COMMAND powershell Expand-Archive "\"${DOWNLOADS_DIR}/gdal_bins.zip\"" "\"${DOWNLOADS_DIR}/gdal\"")
    endif()

    set(GDAL_LIBRARY "${DOWNLOADS_DIR}/gdal/lib/gdal_i.lib")
    set(GDAL_INCLUDE_DIR "${DOWNLOADS_DIR}/gdal/include")
    set(CURL_LIBRARY "${DOWNLOADS_DIR}/gdal/lib/libcurl_imp.lib")
    set(CURL_INCLUDE_DIR "${DOWNLOADS_DIR}/gdal/include")
    set(SPATIALITE_LIBRARY "${DOWNLOADS_DIR}/gdal/lib/spatialite_i.lib")
    set(SQLITE3_LIBRARY "${DOWNLOADS_DIR}/gdal/lib/sqlite3_i.lib")
    
    set(CURL_DIR "\"${DOWNLOADS_DIR}/gdal\"")

    # Fix: auto_ptr is not a member of std
    add_compile_definitions(_HAS_AUTO_PTR_ETC=1)
endif()

find_package(GDAL REQUIRED)
find_package(CURL REQUIRED)
find_package(SpatiaLite REQUIRED)
find_package(Sqlite3 REQUIRED)
include_directories(${GDAL_INCLUDE_DIR})

if (NOT WIN32)
    set(STDPPFS_LIBRARY stdc++fs)
endif()

set(LINK_LIBRARIES ${SPATIALITE_LIBRARY} ${SQLITE3_LIBRARY} ${STDPPFS_LIBRARY} exiv2lib cctz::cctz ${GDAL_LIBRARY} ${CURL_LIBRARY})
set(CMD_SRC_LIST ${CMAKE_CURRENT_SOURCE_DIR}/src/cmd/main.cpp)

add_subdirectory("src")
add_subdirectory("vendor")

include_directories("src")
include_directories("${CMAKE_BINARY_DIR}")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/vendor")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/vendor/cctz/include")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/vendor/exiv2/include")

add_library(${PROJECT_NAME} SHARED ${SRC_LIST})
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)

if(NOT WIN32)
    # Tell the linker how to resolve library names
    target_link_libraries(${PROJECT_NAME} PRIVATE "-lspatialite -lsqlite3 -lgdal -lcurl -lexiv2 -L\"${CMAKE_BINARY_DIR}/lib\"")
endif()

# set PLOG to PLOG_GLOBAL/PLOG_EXPORT to share instances across modules (and export on Windows)
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLOG_EXPORT)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLOG_GLOBAL)
endif()

if (WIN32)
    add_custom_command(TARGET "${PROJECT_NAME}" POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different  $<TARGET_FILE:ddb> "${CMAKE_BINARY_DIR}/ddb.dll"
        COMMENT "Created ${CMAKE_BINARY_DIR}/ddb.dll"
    )

    add_custom_command(TARGET "${PROJECT_NAME}" POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different  $<TARGET_FILE:exiv2lib> "${CMAKE_BINARY_DIR}/exiv2.dll"
        COMMENT "Created ${CMAKE_BINARY_DIR}/exiv2.dll"
    )

    file(GLOB GDAL_DLLS "${DOWNLOADS_DIR}/gdal/bin/*.dll")
    file(COPY ${GDAL_DLLS} DESTINATION ${CMAKE_BINARY_DIR})

    file(COPY "${DOWNLOADS_DIR}/gdal/bin/proj6/share/proj.db" DESTINATION ${CMAKE_BINARY_DIR})

    # Fix linking
    target_link_libraries("${PROJECT_NAME}" ${LINK_LIBRARIES})
else()
    # Copy proj.db into build directory
    file(COPY "/usr/share/proj/proj.db" DESTINATION ${CMAKE_BINARY_DIR})
endif()

file(GLOB DATA_FILES "${CMAKE_CURRENT_SOURCE_DIR}/ddb_data/*")
list(FILTER DATA_FILES EXCLUDE REGEX "^.git$")
file(COPY ${DATA_FILES} DESTINATION ${CMAKE_BINARY_DIR})

# Copy platform files
set(PLATFORM "unix")
if (WIN32)
    set(PLATFORM "win32")
endif()
file(GLOB PLATFORM_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/${PLATFORM}/*")
file(COPY ${PLATFORM_FILES} DESTINATION "${CMAKE_BINARY_DIR}")

if (NOT CMAKE_JS_VERSION)
    add_executable("${PROJECT_NAME}cmd" ${CMD_SRC_LIST})
    set_target_properties("${PROJECT_NAME}cmd" PROPERTIES CXX_STANDARD 17)
    target_link_libraries("${PROJECT_NAME}cmd" ${PROJECT_NAME} ${LINK_LIBRARIES})

    if (WIN32)
        # Enable globbing/path expansion
        SET_TARGET_PROPERTIES("${PROJECT_NAME}cmd" PROPERTIES LINK_FLAGS "/link setargv.obj")
    endif()

    # set PLOG to PLOG_GLOBAL/PLOG_IMPORT to share instances across modules (and import on Windows)
    if(WIN32)
        target_compile_definitions("${PROJECT_NAME}cmd" PRIVATE PLOG_IMPORT)
    else()
        target_compile_definitions("${PROJECT_NAME}cmd" PRIVATE PLOG_GLOBAL)
    endif()

    # On Unix, rename ddbcmd --> ddb
    # On Windows, keep ddbcmd since we'll use ddb.bat to launch it
    set(DDBCMD_OUTFILE "ddb")
    if (WIN32)
        set(DDBCMD_OUTFILE "ddbcmd.exe")
    endif()

    add_custom_command(TARGET "${PROJECT_NAME}cmd" POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ddbcmd> ${CMAKE_BINARY_DIR}/${DDBCMD_OUTFILE}
        COMMENT "Created ${CMAKE_BINARY_DIR}/${DDBCMD_OUTFILE}"
    )
endif()

# Nodejs bindings
if (CMAKE_JS_VERSION)
    message ("Building Node.js bindings")

    # Don't build distribution target
    set(SKIP_DIST ON)

    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/nan")
        message("Installing nan...")
        execute_process(COMMAND npm install nan)
    endif()

    include_directories(${CMAKE_JS_INC})

    file(GLOB NODE_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/nodejs/*.cc")
    add_library("node-${PROJECT_NAME}" SHARED ${NODE_SOURCE_FILES} ${CMAKE_JS_SRC})
    set_target_properties("node-${PROJECT_NAME}" PROPERTIES PREFIX "" SUFFIX ".node")
    set_target_properties("node-${PROJECT_NAME}" PROPERTIES CXX_STANDARD 17)
    target_link_libraries("node-${PROJECT_NAME}" ${CMAKE_JS_LIB} ${PROJECT_NAME} ${LINK_LIBRARIES})

    if (WIN32)
        add_custom_command(TARGET "node-${PROJECT_NAME}" POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:node-ddb> ${CMAKE_BINARY_DIR}/node-ddb.node
            COMMENT "Created ${CMAKE_BINARY_DIR}/node-ddb.node")
    endif()
endif()

if(BUILD_TESTING)
    add_subdirectory("test")
endif()

# Distribution

function(create_zip output_file input_files working_dir)
    add_custom_command(
        COMMAND ${CMAKE_COMMAND} -E tar "cf" "${output_file}" --format=zip -- ${input_files}
        WORKING_DIRECTORY "${working_dir}"
        OUTPUT  "${output_file}"
        DEPENDS ${input_files}
        COMMENT "Zipping to ${output_file}."
    )
endfunction()

function(exodus_pack exodus_exe_path output_file exe_file dist_files working_dir)
    set(DEPS_PARAMS "")
    foreach(F ${dist_files})
        list(APPEND DEPS_PARAMS "-a")
        list(APPEND DEPS_PARAMS "${F}")
    endforeach()

    get_filename_component(DIST_DIRECTORY ${output_file} DIRECTORY)
    file(MAKE_DIRECTORY ${DIST_DIRECTORY})

    add_custom_command(
        COMMAND ${exodus_exe_path} "${exe_file}" "-t" "-o" "${output_file}" ${DEPS_PARAMS}
        WORKING_DIRECTORY "${working_dir}"
        OUTPUT  "${output_file}"
        DEPENDS ${dist_files} ${exe_file}
        COMMENT "Packing to ${output_file}."
    )
endfunction()

set(PLATFORM_NAME "linux")
set(OUTPUT_DIST_EXT "tgz")
if (WIN32)
    set(PLATFORM_NAME "windows")
    set(OUTPUT_DIST_EXT "zip")
    # TODO: Mac
endif()

set(OUTPUT_DIST_FILE "${PROJECT_NAME}-${APP_VERSION}-${PLATFORM_NAME}.${OUTPUT_DIST_EXT}")
set(SUPPORT_FILES "proj.db" "sensor_data.sqlite" "timezone21.bin")

set(DIST_FILES "")

if (WIN32)
    # Add DLLs (added at cmake build time)
    file(GLOB DIST_FILES "${CMAKE_BINARY_DIR}/*.dll")
    
    list(APPEND DIST_FILES "${DDBCMD_OUTFILE}")
    list(APPEND DIST_FILES "${CMAKE_BINARY_DIR}/ddb.bat")

    # These are added after build so they are not available with GLOB
    list(APPEND DIST_FILES "${CMAKE_BINARY_DIR}/ddb.dll")
endif()

foreach(F ${SUPPORT_FILES})
    list(APPEND DIST_FILES "${CMAKE_BINARY_DIR}/${F}")
endforeach()

# On Windows we just zip
if (WIN32)
    create_zip("${CMAKE_BINARY_DIR}/dist/${OUTPUT_DIST_FILE}" "${DIST_FILES}" "${CMAKE_BINARY_DIR}")
else()
    # On Linux we use exodus
    find_program(EXODUS exodus "$ENV{HOME}/.local/bin/" "/usr/local/bin" REQUIRED)
    if (${EXODUS} STREQUAL "EXODUS-NOTFOUND")
        set(SKIP_DIST ON)
        message(WARNING "exodus not found. Install it via: pip install --user exodus if you want to generate a distributable version of ddb")
    else()
        exodus_pack(${EXODUS} "${CMAKE_BINARY_DIR}/dist/${OUTPUT_DIST_FILE}" "${DDBCMD_OUTFILE}" "${DIST_FILES}" "${CMAKE_BINARY_DIR}")
    endif()
endif()

if (NOT SKIP_DIST)
    add_custom_target("dist" DEPENDS "${PROJECT_NAME}cmd" "${CMAKE_BINARY_DIR}/dist/${OUTPUT_DIST_FILE}")
endif()

# Installation

if (NOT WIN32)
install(TARGETS ${PROJECT_NAME})
install(PROGRAMS "${CMAKE_BINARY_DIR}/ddbcmd" DESTINATION bin RENAME ddb)
install(FILES ${DIST_FILES} DESTINATION share/ddb)
endif()
