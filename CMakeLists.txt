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

find_package(exiv2 REQUIRED CONFIG NAMES exiv2)
find_package(GeographicLib REQUIRED)
find_package(GDAL REQUIRED)
find_package(CURL REQUIRED)
include_directories(${GDAL_INCLUDE_DIR})

set(LINK_LIBRARIES pthread dl spatialite sqlite3 stdc++fs exiv2 cctz::cctz ${GeographicLib_LIBRARIES} ${GDAL_LIBRARY} ${CURL_LIBRARY})

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
    if (WIN32)
        # TODO
    else()
       target_link_libraries("${PROJECT_NAME}cmd" ${PROJECT_NAME} ${LINK_LIBRARIES})
    endif()
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
