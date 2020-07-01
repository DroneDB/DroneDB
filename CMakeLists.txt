cmake_minimum_required(VERSION 2.8)

cmake_policy(SET CMP0048 NEW)
project(ddb)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# set(CMAKE_JS_VERSION ON)
# set(CMAKE_BUILD_TYPE "Debug")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING
      "Choose the type of build, options are: Debug Release
RelWithDebInfo MinSizeRel."
      FORCE)
endif(NOT CMAKE_BUILD_TYPE)

find_program(CCACHE_FOUND ccache)
if(CCACHE_FOUND)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
endif(CCACHE_FOUND)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(${CMAKE_ROOT}/Modules/ExternalProject.cmake)
set(DOWNLOADS_DIR "${CMAKE_BINARY_DIR}/downloads")

if (WIN32)
    # Avoid __declspec definitions in code, requires CMake >= 3.3
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    #unset(CMAKE_IMPORT_LIBRARY_SUFFIX)

    # Download GDAL/CURL/sqlite3/spatialite libraries
    if (NOT EXISTS "${DOWNLOADS_DIR}/gdal_libs.zip")
        message("Downloading GDAL libs...")
        file(DOWNLOAD "http://download.gisinternals.com/sdk/downloads/release-1911-x64-gdal-3-0-4-mapserver-7-4-3-libs.zip" "${DOWNLOADS_DIR}/gdal_libs.zip")
    endif()

    if (NOT EXISTS "${DOWNLOADS_DIR}/gdal_bins.zip")
        message("Downloading GDAL binaries...")
        file(DOWNLOAD "http://download.gisinternals.com/sdk/downloads/release-1911-x64-gdal-3-0-4-mapserver-7-4-3.zip" "${DOWNLOADS_DIR}/gdal_bins.zip")
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

set(EXIV_CMAKE_BUILD_FLAGS "-DEXIV2_BUILD_SAMPLES=OFF" "-DEXIV2_BUILD_EXIV2_COMMAND=OFF")
if (WIN32)
    list(APPEND EXIV_CMAKE_BUILD_FLAGS "-DEXPAT_LIBRARY=${DOWNLOADS_DIR}/gdal/lib/expat.lib" "-DEXPAT_INCLUDE_DIR=${DOWNLOADS_DIR}/gdal/include" "-DZLIB_LIBRARY=${DOWNLOADS_DIR}/gdal/lib/zlib.lib" "-DZLIB_INCLUDE_DIR=${DOWNLOADS_DIR}/gdal/include")
endif()

ExternalProject_Add(exiv2
  PREFIX            ${CMAKE_BINARY_DIR}/exiv2
  TMP_DIR           ${CMAKE_BINARY_DIR}/exiv2-tmp
  STAMP_DIR         ${CMAKE_BINARY_DIR}/exiv2-stamp
  #--Download step--------------
  DOWNLOAD_DIR      ${DOWNLOADS_DIR}
  GIT_REPOSITORY    https://github.com/uav4geo/exiv2
  GIT_TAG           master
  #--Update/Patch step----------
  UPDATE_COMMAND    ""
  #--Configure step-------------
  SOURCE_DIR        ${CMAKE_BINARY_DIR}/exiv2
  CMAKE_ARGS ${EXIV_CMAKE_BUILD_FLAGS} --target exiv2lib
  #--Build step-----------------
  BINARY_DIR        ${CMAKE_BINARY_DIR}/exiv2-build
  #--Install step---------------
  INSTALL_COMMAND    ""
  #--Output logging-------------
  LOG_DOWNLOAD      OFF
  LOG_CONFIGURE     OFF
  LOG_BUILD         OFF
)
include_directories(${CMAKE_BINARY_DIR}/exiv2-build)
include_directories(${CMAKE_BINARY_DIR}/exiv2/include)


set(EXIV2_LIB "${CMAKE_BINARY_DIR}/exiv2-build/lib/libexiv2.so")
if (WIN32)
    # TODO: FIX!
    set(EXIV2_LIB ${CMAKE_BINARY_DIR}/exiv2-build/lib/Debug/exiv2.lib)
endif()

find_package(GDAL REQUIRED)
find_package(CURL REQUIRED)
find_package(SpatiaLite REQUIRED)
find_package(Sqlite3 REQUIRED)
include_directories(${GDAL_INCLUDE_DIR})

#dl
if (NOT WIN32)
    set(STDPPFS_LIBRARY stdc++fs)
endif()

set(LINK_LIBRARIES ${SPATIALITE_LIBRARY} ${SQLITE3_LIBRARY} ${STDPPFS_LIBRARY} ${EXIV2_LIB} cctz::cctz ${GDAL_LIBRARY} ${CURL_LIBRARY})

set(SRC_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logger.cpp
    )
set(CMD_SRC_LIST ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    )
add_subdirectory("cmd")
add_subdirectory("classes")
add_subdirectory("libs")
add_subdirectory("vendor")

include_directories("${CMAKE_CURRENT_SOURCE_DIR}/vendor")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/vendor/cctz/include")

add_library(${PROJECT_NAME} SHARED ${SRC_LIST})
add_dependencies(${PROJECT_NAME} exiv2)
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)

if (NOT CMAKE_JS_VERSION)
    add_executable("${PROJECT_NAME}cmd" ${CMD_SRC_LIST})
    set_target_properties("${PROJECT_NAME}cmd" PROPERTIES CXX_STANDARD 17)
    target_link_libraries("${PROJECT_NAME}cmd" ${PROJECT_NAME} ${LINK_LIBRARIES})

    # Rename ddbcmd --> ddb
    add_custom_command(TARGET "${PROJECT_NAME}cmd" POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ddbcmd> ${CMAKE_BINARY_DIR}/ddb${EXE_EXT}
        COMMENT "Created ${CMAKE_BINARY_DIR}/ddb${EXE_EXT}"
    )

    set(EXE_EXT "")
    if (WIN32)
        set(EXE_EXT ".exe")

        # Copy DLLs
        add_custom_command(TARGET "${PROJECT_NAME}cmd" POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different  $<TARGET_FILE:ddb> "${CMAKE_BINARY_DIR}/ddb.dll"
            COMMENT "Created ${CMAKE_BINARY_DIR}/ddb.dll"
        )

        add_custom_command(
            TARGET "${PROJECT_NAME}" PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${CMAKE_BINARY_DIR}/exiv2-build/bin/exiv2.dll"
                    "${CMAKE_BINARY_DIR}/"
            COMMENT "Copied ${CMAKE_BINARY_DIR}/exiv2-build/bin/exiv2.dll"
        )

        file(GLOB GDAL_DLLS "${DOWNLOADS_DIR}/gdal/bin/*.dll")
        file(COPY ${GDAL_DLLS} DESTINATION ${CMAKE_BINARY_DIR})

        # Fix linking
        target_link_libraries("${PROJECT_NAME}" ${LINK_LIBRARIES})
    endif()


    file(GLOB DATA_FILES "${CMAKE_CURRENT_SOURCE_DIR}/ddb_data/*")
    file(COPY ${DATA_FILES} DESTINATION ${CMAKE_BINARY_DIR})

else()
    
    if (WIN32)

        add_custom_command(
            TARGET "${PROJECT_NAME}" PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${CMAKE_BINARY_DIR}/exiv2-build/bin/exiv2.dll"
                    "${CMAKE_BINARY_DIR}/"
            COMMENT "Copied ${CMAKE_BINARY_DIR}/exiv2-build/bin/exiv2.dll"
        )

        file(GLOB GDAL_DLLS "${DOWNLOADS_DIR}/gdal/bin/*.dll")
        file(COPY ${GDAL_DLLS} DESTINATION ${CMAKE_BINARY_DIR})

        # Fix linking
        target_link_libraries("${PROJECT_NAME}" ${LINK_LIBRARIES})
    endif()


    file(GLOB DATA_FILES "${CMAKE_CURRENT_SOURCE_DIR}/ddb_data/*")
    file(COPY ${DATA_FILES} DESTINATION ${CMAKE_BINARY_DIR})

endif()

# json1 extension
#add_library(json1 SHARED vendor/ext/json1.c)
#set_target_properties(json1 PROPERTIES PREFIX "")
#set_property(TARGET json1 PROPERTY POSITION_INDEPENDENT_CODE ON)

# Nodejs bindings
if (CMAKE_JS_VERSION)

    message ("BUILDING WITH NODE")

    set(BUILD_TESTING OFF)
    include_directories(${CMAKE_JS_INC})

    # Statement above doesn't include directories (QtCreator bug?)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(${CMAKE_CURRENT_SOURCE_DIR})
        include_directories("${CMAKE_CURRENT_SOURCE_DIR}/node_modules/nan")
        include_directories("C:/Users/IT 2/.cmake-js/node-x64/v12.16.1/include/node")
    endif()

    file(COPY ${DATA_FILES} DESTINATION "${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}")

    file(GLOB NODE_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/nodejs/*.cc")
    add_library("node-${PROJECT_NAME}" SHARED ${NODE_SOURCE_FILES})
    set_target_properties("node-${PROJECT_NAME}" PROPERTIES PREFIX "" SUFFIX ".node")
    set_target_properties("node-${PROJECT_NAME}" PROPERTIES CXX_STANDARD 17)
    target_link_libraries("node-${PROJECT_NAME}" ${CMAKE_JS_LIB} ${PROJECT_NAME} ${LINK_LIBRARIES})
endif()

if(BUILD_TESTING)
    add_subdirectory("test")
endif()
