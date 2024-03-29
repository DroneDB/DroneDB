cmake_minimum_required(VERSION 2.8)

cmake_policy(SET CMP0048 NEW)

# Used to show all CMake constants
#set (CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_LIST_DIR}/CompilerOptions.cmake")
project(ddb)

# Read version
file(READ "package.json" PACKAGE_JSON)
string(REGEX MATCH "\"version\": \"[0-9]+\\.[0-9]+\\.[0-9]+" APP_VERSION ${PACKAGE_JSON})
string(SUBSTRING ${APP_VERSION} 12 -1 APP_VERSION)

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
add_compile_options("-DAPP_VERSION=\"${DAPP_VERSION}\"")
add_compile_options("-DAPP_REVISION=\"${DAPP_REVISION}\"")

if (WIN32)
    # Disable MS warnings
    add_compile_options("-D_CRT_SECURE_NO_WARNINGS=1")

    add_definitions(-DNOMINMAX)
endif()

# Disable GNU warnings (for exiv2)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options("-Wno-deprecated-declarations")
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (UNIX)
    find_program(HOMEBREW_EXECUTABLE brew)
    mark_as_advanced(FORCE HOMEBREW_EXECUTABLE)
    if (HOMEBREW_EXECUTABLE)
        # Detected a Homebrew install, query for its install prefix.
        execute_process(COMMAND ${HOMEBREW_EXECUTABLE} --prefix
            OUTPUT_VARIABLE HOMEBREW_INSTALL_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        message(STATUS "Detected Homebrew with install prefix: "
            "${HOMEBREW_INSTALL_PREFIX}, adding to CMake search paths.")
        list(APPEND CMAKE_PREFIX_PATH "${HOMEBREW_INSTALL_PREFIX}")
        link_directories(${HOMEBREW_INSTALL_PREFIX}/lib)
    endif()

    if (APPLE)
        set(SQLITE3_LIBRARY "${HOMEBREW_INSTALL_PREFIX}/opt/sqlite3/lib/libsqlite3.dylib")

        # For cctz
        find_library(CoreFoundation CoreFoundation)
    endif()
endif()

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
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake" "${CMAKE_SOURCE_DIR}/vendor/exiv2/cmake")
include(${CMAKE_ROOT}/Modules/ExternalProject.cmake)
set(DOWNLOADS_DIR "${CMAKE_BINARY_DIR}/downloads")

if (WIN32)
    function (download_dependency target_zip url)
        if (NOT EXISTS "${DOWNLOADS_DIR}/${target_zip}")
            message("Downloading ${target_zip}...")
            file(DOWNLOAD "${url}" "${DOWNLOADS_DIR}/${target_zip}" STATUS result)
            message("Download result: ${result}")
        endif()
    endfunction()

    function(extract_dependency target_zip path_check dest_dir )
        if (NOT EXISTS "${DOWNLOADS_DIR}/${path_check}")
            message("Extracting ${dest_dir}...")
            execute_process(COMMAND powershell Expand-Archive "\"${DOWNLOADS_DIR}/${target_zip}\"" "\"${DOWNLOADS_DIR}/${dest_dir}\" -Force")
        endif()
    endfunction()

    # Download GDAL/CURL/sqlite3/spatialite/PDAL and supporting libraries
    download_dependency("gdal_libs.zip" "https://github.com/DroneDB/DroneDB/releases/download/v0.9.13/release-1928-x64-gdal-3-3-2-mapserver-7-6-4-libs.zip")
    download_dependency("gdal_bins.zip" "https://github.com/DroneDB/DroneDB/releases/download/v0.9.13/release-1928-x64-gdal-3-3-2-mapserver-7-6-4-sans-sqlite3.zip")
    download_dependency("laszip.zip" "https://github.com/DroneDB/ddb_data/releases/download/v0.9.2/laszip-3-4-3.zip")
    download_dependency("geotiff.zip" "https://github.com/DroneDB/DroneDB/releases/download/v0.9.13/release-1928-x64-gdal-3-3-2-mapserver-7-6-4-geotiff.zip")
    download_dependency("pdal.zip" "https://github.com/DroneDB/DroneDB/releases/download/v0.9.13/release-1928-x64-gdal-3-3-2-mapserver-7-6-4-pdal-2-3-0.zip")
    download_dependency("zlib.zip" "https://github.com/DroneDB/ddb_data/releases/download/v0.9.2/zlib-1-2-11.zip")
    download_dependency("libzip.zip" "https://github.com/DroneDB/ddb_data/releases/download/v0.9.2/zlib-1-2-11-libzip-1-8-0.zip")
    download_dependency("sqlite3.zip" "https://github.com/DroneDB/ddb_data/releases/download/v0.9.2/sqlite-dll-win64-x64-3360000.zip")
    download_dependency("nxs.zip" "https://github.com/DroneDB/libnexus/releases/download/v1.0.0/nxs-windows-64bit.zip")
    
    extract_dependency("gdal_libs.zip" "gdal/include" "gdal")
    extract_dependency("gdal_bins.zip" "gdal/bin" "gdal")
    extract_dependency("laszip.zip" "laszip/include" "laszip")
    extract_dependency("geotiff.zip" "geotiff/include" "geotiff")
    extract_dependency("pdal.zip" "pdal/include" "pdal")
    extract_dependency("zlib.zip" "zlib/include" "zlib")
    extract_dependency("libzip.zip" "libzip/include" "libzip")
    extract_dependency("sqlite3.zip" "sqlite3" "sqlite3")
    extract_dependency("nxs.zip" "nxs" "nxs")
    
    set(GDAL_LIBRARY "${DOWNLOADS_DIR}/gdal/lib/gdal_i.lib")
    set(GDAL_INCLUDE_DIR "${DOWNLOADS_DIR}/gdal/include")
    set(CURL_LIBRARY "${DOWNLOADS_DIR}/gdal/lib/libcurl_imp.lib")
    set(CURL_INCLUDE_DIR "${DOWNLOADS_DIR}/gdal/include")
    set(SPATIALITE_LIBRARY "${DOWNLOADS_DIR}/gdal/lib/spatialite_i.lib")
    set(SQLITE3_LIBRARY "${DOWNLOADS_DIR}/gdal/lib/sqlite3_i.lib")
    set(CURL_DIR "\"${DOWNLOADS_DIR}/gdal\"")
    set(PDAL_LIBRARIES "${DOWNLOADS_DIR}/pdal/lib/pdalcpp.lib" "${DOWNLOADS_DIR}/pdal/lib/pdal_util.lib" "${DOWNLOADS_DIR}/pdal/lib/libpdal_plugin_kernel_fauxplugin.lib")
    set(PDAL_INCLUDE_DIR "${DOWNLOADS_DIR}/pdal/include")
    set(Zip_LIBRARIES "${DOWNLOADS_DIR}/libzip/lib/zip.lib")
    set(Zip_INCLUDE_DIRS "${DOWNLOADS_DIR}/libzip/include")
    set(NXS_LIBRARIES "${DOWNLOADS_DIR}/nxs/lib/nxs.lib")
    set(NXS_INCLUDE_DIR "${DOWNLOADS_DIR}/nxs/include")
    set(NXS_DIR "${DOWNLOADS_DIR}/nxs/share/cmake/nxs")

    include_directories("${DOWNLOADS_DIR}/laszip/include")

    # Fix: auto_ptr is not a member of std
    add_compile_definitions(_HAS_AUTO_PTR_ETC=1)
endif()

find_package(GDAL REQUIRED)
find_package(CURL REQUIRED)
find_package(SpatiaLite REQUIRED)
find_package(Sqlite3 REQUIRED)
find_package(PDAL REQUIRED)
find_package(Threads REQUIRED)
find_package(Zip REQUIRED)
find_package(NXS)
include_directories(${Zip_INCLUDE_DIRS})
include_directories(${GDAL_INCLUDE_DIR})
include_directories(${PDAL_INCLUDE_DIR})
include_directories(${PDAL_INCLUDE_DIR}/pdal)
include_directories(${NXS_INCLUDE_DIR})

if (NOT WIN32 AND NOT APPLE)
    set(STDPPFS_LIBRARY stdc++fs)
endif()

set(LINK_LIBRARIES ${SPATIALITE_LIBRARY} ${SQLITE3_LIBRARY} ${STDPPFS_LIBRARY} exiv2lib ${GDAL_LIBRARY} ${CURL_LIBRARY} ${PDAL_LIBRARIES} ${Zip_LIBRARIES} ${NXS_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
set(CMD_SRC_LIST ${CMAKE_CURRENT_SOURCE_DIR}/src/cmd/main.cpp)

add_subdirectory("src")
add_subdirectory("vendor")

set_target_properties(exiv2lib PROPERTIES CXX_STANDARD 17)


include_directories("src")
include_directories("${CMAKE_BINARY_DIR}")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/vendor")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/vendor/cctz/include")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/vendor/exiv2/include")

if (HOMEBREW_INSTALL_PREFIX)
    # Some weird stuff happens with libspatialite on macOS
    # if we build this as a shared library
    add_library(${PROJECT_NAME} STATIC ${SRC_LIST})
else()
    add_library(${PROJECT_NAME} SHARED ${SRC_LIST})
endif()
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)
add_dependencies(${PROJECT_NAME} exiv2lib)

if (NOT NXS_FOUND)
    message(WARNING "Nexus support will not be built")
    add_definitions(-DNO_NEXUS=1)
endif()

if(NOT WIN32)
    # Tell the linker how to resolve library names
    set(LINKER_LIBS "-lspatialite -lsqlite3 -lgdal -lcurl -lpdalcpp -lzip")
    if (NXS_FOUND)
        set(LINKER_LIBS "${LINKER_LIBS} -lnxs")
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE "${LINKER_LIBS} -L\"${CMAKE_BINARY_DIR}/lib\"")
    target_link_libraries(${PROJECT_NAME} PRIVATE exiv2)

    # Add segvcatch flags
    target_compile_options(${PROJECT_NAME} PRIVATE -fexceptions -fnon-call-exceptions)

    # cctz
    if (APPLE)
        target_link_libraries(${PROJECT_NAME} PUBLIC ${CoreFoundation}) 
    endif()
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

    function(copy_dlls pattern)
        file(GLOB COPY_DLLS "${DOWNLOADS_DIR}/${pattern}")
        file(COPY ${COPY_DLLS} DESTINATION ${CMAKE_BINARY_DIR})
    endfunction()

    copy_dlls("gdal/bin/*.dll")
    copy_dlls("laszip/bin/*.dll")
    copy_dlls("geotiff/bin/*.dll")
    copy_dlls("pdal/bin/*.dll")
    copy_dlls("zlib/bin/*.dll")
    copy_dlls("libzip/bin/*.dll")
    copy_dlls("sqlite3/bin/*.dll")
    copy_dlls("nxs/lib/*.dll")

    file(COPY "${DOWNLOADS_DIR}/gdal/bin/proj7/share/proj.db" DESTINATION ${CMAKE_BINARY_DIR})

    # Fix linking
    target_link_libraries("${PROJECT_NAME}" ${LINK_LIBRARIES})
elseif(HOMEBREW_INSTALL_PREFIX)
    file(COPY "${HOMEBREW_INSTALL_PREFIX}/share/proj/proj.db" DESTINATION ${CMAKE_BINARY_DIR})
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
set(SUPPORT_FILES "proj.db" "sensor_data.sqlite" "timezone21.bin" "curl-ca-bundle.crt")
if (WIN32)
    # Linux usually comes with a zoneinfo database already
    list(APPEND SUPPORT_FILES "zoneinfo")
endif()

set(DIST_FILES "")

if (WIN32)
    # Add DLLs (added at cmake build time)
    file(GLOB DIST_FILES "${CMAKE_BINARY_DIR}/*.dll")
    
    list(APPEND DIST_FILES "${DDBCMD_OUTFILE}")
    list(APPEND DIST_FILES "${CMAKE_BINARY_DIR}/ddb.bat")

    # These are added after build so they are not available with GLOB
    list(APPEND DIST_FILES "${CMAKE_BINARY_DIR}/ddb.dll" "${CMAKE_BINARY_DIR}/exiv2.dll")
endif()

foreach(F ${SUPPORT_FILES})
    list(APPEND DIST_FILES "${CMAKE_BINARY_DIR}/${F}")
endforeach()

# On Windows we just zip
if (WIN32)
    create_zip("${CMAKE_BINARY_DIR}/dist/${OUTPUT_DIST_FILE}" "${DIST_FILES}" "${CMAKE_BINARY_DIR}")
else()
    # On Linux we use exodus
    find_program(EXODUS exodus "$ENV{HOME}/.local/bin/" "/usr/local/bin")
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
install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION lib)
install(PROGRAMS "${CMAKE_BINARY_DIR}/ddbcmd" DESTINATION bin RENAME ddb)
install(FILES ${DIST_FILES} DESTINATION share/ddb)
endif()
