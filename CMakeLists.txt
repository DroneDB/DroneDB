cmake_minimum_required(VERSION 2.8)

project(ddb)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING
      "Choose the type of build, options are: Debug Release
RelWithDebInfo MinSizeRel."
      FORCE)
endif(NOT CMAKE_BUILD_TYPE)


set(SRC_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/database.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/exceptions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/statement.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/index.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/exif.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/timezone.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hash.cpp


    )
add_subdirectory("cmd")
add_subdirectory("libs")

include_directories("${CMAKE_CURRENT_SOURCE_DIR}/libs")
add_executable(${PROJECT_NAME} ${SRC_LIST})

file(GLOB DATA_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/ddb_data/*"
)
file(COPY ${DATA_FILES} DESTINATION ${CMAKE_BINARY_DIR})

find_package(exiv2 REQUIRED CONFIG NAMES exiv2)

if (WIN32)
    # TODO
else()
   target_link_libraries(${PROJECT_NAME} pthread dl spatialite sqlite3 stdc++fs exiv2 cctz::cctz)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 17
    )

