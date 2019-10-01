cmake_minimum_required(VERSION 2.8)

project(ddb)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

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


set(SRC_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logger.cpp

    )
add_subdirectory("cmd")
add_subdirectory("classes")
add_subdirectory("libs")
add_subdirectory("vendor")

include_directories("${CMAKE_CURRENT_SOURCE_DIR}/vendor")
include_directories("${CMAKE_BINARY_DIR}/install/include")
add_executable(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp ${SRC_LIST})

file(GLOB DATA_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/ddb_data/*"
)
file(COPY ${DATA_FILES} DESTINATION ${CMAKE_BINARY_DIR})

find_package(exiv2 REQUIRED CONFIG NAMES exiv2)
find_package(GeographicLib REQUIRED)

set(LINK_LIBRARIES pthread dl spatialite sqlite3 stdc++fs exiv2 cctz::cctz ${GeographicLib_LIBRARIES})

if (WIN32)
    # TODO
else()
   target_link_libraries(${PROJECT_NAME} ${LINK_LIBRARIES})
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 17
    )

if(BUILD_TESTING)
add_subdirectory("test")
endif()

# json1 extension
#add_library(json1 SHARED vendor/ext/json1.c)
#set_target_properties(json1 PROPERTIES PREFIX "")
#set_property(TARGET json1 PROPERTY POSITION_INDEPENDENT_CODE ON)
