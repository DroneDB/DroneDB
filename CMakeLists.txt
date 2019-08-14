cmake_minimum_required(VERSION 2.8)

project(ddb)

set(SRC_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/database.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/exceptions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/statement.cpp

    )
add_subdirectory("cmd")
add_subdirectory("libs")

include_directories("${CMAKE_CURRENT_SOURCE_DIR}/libs")
add_executable(${PROJECT_NAME} ${SRC_LIST})

if (WIN32)

else()
   target_link_libraries(${PROJECT_NAME} pthread dl spatialite sqlite3 stdc++fs)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 17
    )

