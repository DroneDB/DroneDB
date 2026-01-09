if(BUILD_TESTING)

    message(STATUS "BUILD_TESTING is ON")

    find_package(GTest CONFIG REQUIRED)
    find_package(cpr CONFIG REQUIRED)
    find_package(GDAL CONFIG REQUIRED)
    find_package(tinyobjloader CONFIG REQUIRED)

    file(GLOB TEST_SOURCES "*.cpp")

    # Create the test executable
    add_executable(ddbtest ${TEST_SOURCES})

    # Link the library and testing framework
    target_link_libraries(ddbtest PRIVATE ${PROJECT_NAME} GTest::gtest GTest::gtest_main cpr::cpr GDAL::GDAL tinyobjloader::tinyobjloader)

    # set PLOG to PLOG_GLOBAL/PLOG_IMPORT to share instances across modules (and import on Windows)
    if(WIN32)
        target_compile_definitions(ddbtest PRIVATE PLOG_EXPORT)
        set_target_properties(ddbtest PROPERTIES ENABLE_EXPORTS 1)
    else()
        target_compile_definitions(ddbtest PRIVATE PLOG_GLOBAL)
    endif()

    message(STATUS "TESTING: CUSTOM_INCLUDE_DIRS: ${CUSTOM_INCLUDE_DIRS}")

    target_include_directories(ddbtest PRIVATE ${CUSTOM_INCLUDE_DIRS})

    # Copy only the test executable to build directory (not the entire folder)
    # This fixes the issue where copy_directory overwrites cmake_install.cmake
    add_custom_command(TARGET ddbtest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ddbtest>"
            "${CMAKE_BINARY_DIR}"
        COMMENT "Copying ddbtest to build directory"
    )

    # On Windows, also copy runtime DLLs
    if (WIN32)
        add_custom_command(TARGET ddbtest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Staging runtime DLLs for ddbtest..."
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:ddbtest>
                "${CMAKE_BINARY_DIR}"
            COMMAND_EXPAND_LISTS
        )
    endif()

    include(GoogleTest)
    gtest_discover_tests(ddbtest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DISCOVERY_TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
    )

endif()
