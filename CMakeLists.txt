cmake_minimum_required(VERSION 3.16.3)
project(ddb LANGUAGES CXX C)

# Enable C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_MESSAGE_LOG_LEVEL VERBOSE)

message(STATUS "CMAKE_BINARY_DIR: ${CMAKE_BINARY_DIR}")

# Set default build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE Release)
else()
    message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
endif()

set(OUTPUT_DIR "${CMAKE_BINARY_DIR}")

# Define support files directory (these are typically in the main build directory)
set(SUPPORT_DIR "${CMAKE_BINARY_DIR}")
message(STATUS "Using support directory: ${SUPPORT_DIR}")

# Use vcpkg manifest mode

# Determine platform-specific triplet if not already set
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    if(WIN32)
        set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg triplet")
    elseif(APPLE)
        set(VCPKG_TARGET_TRIPLET "x64-osx" CACHE STRING "vcpkg triplet")
    elseif(UNIX)
        set(VCPKG_TARGET_TRIPLET "x64-linux" CACHE STRING "vcpkg triplet")
    else()
        message(WARNING "Unknown platform, using x64-windows as default triplet")
        set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg triplet")
    endif()
endif()
message(STATUS "Using vcpkg triplet: ${VCPKG_TARGET_TRIPLET}")

# On Windows, define _WINSOCKAPI_ to avoid including winsock.h in windows.h
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Consider position-independent code for all targets (especially for shared libs)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

# Find PROJ data folder (vcpkg: share/proj or share/proj4)
find_path(PROJ_DATA_DIR
  NAMES proj.db
  HINTS
    "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/proj"
    "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/proj4"
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/proj"
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/proj4"
  PATH_SUFFIXES share/proj share/proj4
)

if(PROJ_DATA_DIR)
    message(STATUS "Found PROJ data directory: ${PROJ_DATA_DIR}")
else()
    message(WARNING "PROJ data directory not found - proj.db may not be available")
endif()

# Add custom CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Option to build tests
option(BUILD_TESTING "Build unit tests" ON)
include(CTest)

set(SRC_LIST "")

# Add vendor directory
add_subdirectory(vendor)

set(CUSTOM_INCLUDE_DIRS "")

# Add the source code (builds both shared and static libs)
add_subdirectory(src)

# Add applications (main executable)
add_subdirectory(apps)

# Node.js bindings
if (CMAKE_JS_VERSION)
    message(STATUS "Building Node.js bindings")

    # Don't build distribution target
    set(SKIP_DIST ON)

    # Check if nan module exists, if not suggest to install it
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/nan")
        message(WARNING "Node.js NAN module not found. Please run 'npm install' first.")
    endif()

    # Include necessary directories
    include_directories(${CMAKE_JS_INC})

    # Find all Node.js source files
    file(GLOB NODE_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/nodejs/*.cc"
                               "${CMAKE_CURRENT_SOURCE_DIR}/nodejs/*.h")

    # Create Node.js addon library
    add_library("node-${PROJECT_NAME}" SHARED ${NODE_SOURCE_FILES} ${CMAKE_JS_SRC})

    # Configure output properties for the Node.js addon
    set_target_properties("node-${PROJECT_NAME}" PROPERTIES
                         PREFIX ""
                         SUFFIX ${CMAKE_SHARED_LIBRARY_SUFFIX}
                         CXX_STANDARD 17
                         CXX_STANDARD_REQUIRED ON)

    # Link against required libraries
    target_link_libraries("node-${PROJECT_NAME}" ${CMAKE_JS_LIB} ${PROJECT_NAME})

    # Copy the Node.js addon to the build directory with the correct name
    add_custom_command(TARGET "node-${PROJECT_NAME}" POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:node-${PROJECT_NAME}>
                ${CMAKE_BINARY_DIR}/node-${PROJECT_NAME}${CMAKE_SHARED_LIBRARY_SUFFIX}
        COMMENT "Created ${CMAKE_BINARY_DIR}/node-${PROJECT_NAME}${CMAKE_SHARED_LIBRARY_SUFFIX}")
endif()

# Add tests
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation rules
install(TARGETS ${PROJECT_NAME} ${PROJECT_NAME}cmd
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# Install public headers
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/include/
    DESTINATION include
)

# Packaging
include(CPack)
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_VERSION ${APP_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "DroneDB client")
set(CPACK_PACKAGE_MAINTAINER "Luca Di Leo <ldileo@digipa.it>")
set(CPACK_PACKAGE_VENDOR "Digipa S.R.L.")
set(CPACK_PACKAGE_CONTACT "support@dronedb.app")
set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${APP_VERSION}-${CMAKE_SYSTEM_NAME}")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libstdc++6, libc6")

# Windows packaging support files
set(SUPPORT_FILES "proj.db" "sensor_data.sqlite" "timezone21.bin" "curl-ca-bundle.crt")
if (WIN32)
    # Linux usually comes with a zoneinfo database already
    list(APPEND SUPPORT_FILES "zoneinfo")
endif()

set(DIST_FILES "")

foreach(F ${SUPPORT_FILES})
    list(APPEND DIST_FILES "${SUPPORT_DIR}/${F}")
endforeach()

# Create Windows ZIP package
if (WIN32)
    # Create a staging directory for the package contents
    set(PACKAGE_STAGING_DIR "${CMAKE_BINARY_DIR}/package_staging")
    file(MAKE_DIRECTORY ${PACKAGE_STAGING_DIR})

    # Get executables and DLLs from the output directory
    set(EXECUTABLE_FILES "${PROJECT_NAME}cmd.exe")
    foreach(F ${EXECUTABLE_FILES})
        add_custom_command(
            OUTPUT "${PACKAGE_STAGING_DIR}/${F}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUTPUT_DIR}/${F}" "${PACKAGE_STAGING_DIR}/${F}"
            DEPENDS ${PROJECT_NAME}cmd
            COMMENT "Copying ${F} to staging directory"
        )
        list(APPEND STAGED_FILES "${PACKAGE_STAGING_DIR}/${F}")
    endforeach()

    # Copy DLLs to the staging directory
    file(GLOB DLL_FILES "${OUTPUT_DIR}/*.dll")
    foreach(F ${DLL_FILES})
        get_filename_component(FILENAME ${F} NAME)
        add_custom_command(
            OUTPUT "${PACKAGE_STAGING_DIR}/${FILENAME}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${F}" "${PACKAGE_STAGING_DIR}/${FILENAME}"
            DEPENDS ${F}
            COMMENT "Copying ${FILENAME} to staging directory"
        )
        list(APPEND STAGED_FILES "${PACKAGE_STAGING_DIR}/${FILENAME}")
    endforeach()

    # Copy support files to staging directory
    foreach(F ${SUPPORT_FILES})
        if(EXISTS "${SUPPORT_DIR}/${F}")
            add_custom_command(
                OUTPUT "${PACKAGE_STAGING_DIR}/${F}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SUPPORT_DIR}/${F}" "${PACKAGE_STAGING_DIR}/${F}"
                DEPENDS "${SUPPORT_DIR}/${F}"
                COMMENT "Copying ${F} to staging directory"
            )
            list(APPEND STAGED_FILES "${PACKAGE_STAGING_DIR}/${F}")
        endif()
    endforeach()

    # Copy Qt5 plugins directory to staging directory
    if(EXISTS "${OUTPUT_DIR}/plugins")
        add_custom_command(
            OUTPUT "${PACKAGE_STAGING_DIR}/plugins/.copied"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${OUTPUT_DIR}/plugins" "${PACKAGE_STAGING_DIR}/plugins"
            COMMAND ${CMAKE_COMMAND} -E touch "${PACKAGE_STAGING_DIR}/plugins/.copied"
            DEPENDS "${OUTPUT_DIR}/plugins"
            COMMENT "Copying Qt5 plugins directory to staging directory"
        )
        list(APPEND STAGED_FILES "${PACKAGE_STAGING_DIR}/plugins/.copied")
    endif()

    # Copy batch file to staging directory
    if(EXISTS "${SUPPORT_DIR}/ddb.bat")
        add_custom_command(
            OUTPUT "${PACKAGE_STAGING_DIR}/ddb.bat"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SUPPORT_DIR}/ddb.bat" "${PACKAGE_STAGING_DIR}/ddb.bat"
            DEPENDS "${SUPPORT_DIR}/ddb.bat"
            COMMENT "Copying ddb.bat to staging directory"
        )
        list(APPEND STAGED_FILES "${PACKAGE_STAGING_DIR}/ddb.bat")
    endif()

    # Show the files that will be included in the package
    message(STATUS "Files for Windows package: ${STAGED_FILES}")

    set(OUTPUT_ZIP_FILE "${CMAKE_BINARY_DIR}/dist/${PROJECT_NAME}-${APP_VERSION}-${CMAKE_SYSTEM_NAME}.zip")

    # Create the ZIP package from the staging directory
    add_custom_command(
        OUTPUT ${OUTPUT_ZIP_FILE}
        COMMAND ${CMAKE_COMMAND} -E tar "cf" ${OUTPUT_ZIP_FILE} --format=zip .
        WORKING_DIRECTORY ${PACKAGE_STAGING_DIR}
        COMMENT "Creating ZIP package: ${OUTPUT_ZIP_FILE}"
        DEPENDS ${STAGED_FILES}
    )

    add_custom_target(windows_package ALL DEPENDS ${OUTPUT_ZIP_FILE})
endif()


# Final message
message(STATUS "CMake setup complete")
