cmake_minimum_required(VERSION 2.8)

cmake_policy(SET CMP0048 NEW)
project(ddb)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

if (WIN32)
    set(DOWNLOADS_DIR "${CMAKE_BINARY_DIR}/downloads")

    # Download exiv2
    if (NOT EXISTS "${DOWNLOADS_DIR}/exiv2.zip")
        message("Downloading exiv2...")
        file(DOWNLOAD "https://www.exiv2.org/builds/exiv2-0.27.2-2017msvc64.zip" "${DOWNLOADS_DIR}/exiv2.zip")
    endif()

    if (NOT EXISTS "${DOWNLOADS_DIR}/exiv2-0.27.2-2017msvc64")
        message("Extracting exiv2...")
        execute_process(COMMAND powershell Expand-Archive "${DOWNLOADS_DIR}/exiv2.zip" "${DOWNLOADS_DIR}/")
    endif()

    include_directories("${DOWNLOADS_DIR}/exiv2-0.27.2-2017msvc64/include")
    list(APPEND CMAKE_PREFIX_PATH "${DOWNLOADS_DIR}/exiv2-0.27.2-2017msvc64/lib/cmake/exiv2")

    # Download GDAL/CURL/sqlite3/spatialite binaries
    if (NOT EXISTS "${DOWNLOADS_DIR}/gdal.zip")
        message("Downloading GDAL...")
        file(DOWNLOAD "http://download.gisinternals.com/sdk/downloads/release-1911-x64-gdal-3-0-4-mapserver-7-4-3-libs.zip" "${DOWNLOADS_DIR}/gdal.zip")
    endif()

    if (NOT EXISTS "${DOWNLOADS_DIR}/gdal")
        message("Extracting GDAL...")
        execute_process(COMMAND powershell Expand-Archive "${DOWNLOADS_DIR}/gdal.zip" "${DOWNLOADS_DIR}/gdal")
    endif()

    set(GDAL_LIBRARY "${DOWNLOADS_DIR}/gdal/lib/gdal_i.lib")
    set(GDAL_INCLUDE_DIR "${DOWNLOADS_DIR}/gdal/include")
    set(CURL_LIBRARY "${DOWNLOADS_DIR}/gdal/lib/libcurl_imp.lib")
    set(CURL_INCLUDE_DIR "${DOWNLOADS_DIR}/gdal/include")
    set(SPATIALITE_LIBRARY "${DOWNLOADS_DIR}/gdal/lib/spatialite_i.lib")
    set(SQLITE3_LIBRARY "${DOWNLOADS_DIR}/gdal/lib/sqlite3_i.lib")
    
    set(CURL_DIR "${DOWNLOADS_DIR}/gdal")

    # Fix: auto_ptr is not a member of std
    add_compile_definitions(_HAS_AUTO_PTR_ETC=1)
endif()

find_package(exiv2 REQUIRED CONFIG NAMES exiv2)
find_package(GDAL REQUIRED)
find_package(CURL REQUIRED)
find_package(SpatiaLite REQUIRED)
find_package(Sqlite3 REQUIRED)
include_directories(${GDAL_INCLUDE_DIR})

#dl
set(LINK_LIBRARIES pthread ${SPATIALITE_LIBRARY} ${SQLITE3_LIBRARY} stdc++fs exiv2 cctz::cctz ${GDAL_LIBRARY} ${CURL_LIBRARY})

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
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)

if (NOT CMAKE_JS_VERSION)
    add_executable("${PROJECT_NAME}cmd" ${CMD_SRC_LIST})
    set_target_properties("${PROJECT_NAME}cmd" PROPERTIES CXX_STANDARD 17)
    set_target_properties("${PROJECT_NAME}cmd" PROPERTIES OUTPUT_NAME ${PROJECT_NAME})
    target_link_libraries("${PROJECT_NAME}cmd" ${PROJECT_NAME} ${LINK_LIBRARIES})
endif()

file(GLOB DATA_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/ddb_data/*"
)
file(COPY ${DATA_FILES} DESTINATION ${CMAKE_BINARY_DIR})

# json1 extension
#add_library(json1 SHARED vendor/ext/json1.c)
#set_target_properties(json1 PROPERTIES PREFIX "")
#set_property(TARGET json1 PROPERTY POSITION_INDEPENDENT_CODE ON)

# Nodejs bindings
if (CMAKE_JS_VERSION)
  set(BUILD_TESTING OFF)
  include_directories(${CMAKE_JS_INC})

  # Statement above doesn't include directories (QtCreator bug?)
  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  include_directories("/home/piero/Documents/ddb/node_modules/nan")
  include_directories("/home/piero/.cmake-js/node-x64/v13.7.0/include/node")
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
