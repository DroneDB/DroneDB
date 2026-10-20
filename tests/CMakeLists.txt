if(BUILD_TESTING)

    message(STATUS "BUILD_TESTING is ON")

    find_package(GTest CONFIG REQUIRED)
    find_package(cpr CONFIG REQUIRED)
    find_package(GDAL CONFIG REQUIRED)
    find_package(tinyobjloader CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(exiv2 CONFIG REQUIRED)
    find_package(Threads REQUIRED)
    find_package(plog CONFIG REQUIRED)
    find_package(unofficial-sqlite3 CONFIG REQUIRED)

    file(GLOB TEST_SOURCES "*.cpp")

    # Keep the child helper's main outside TEST_SOURCES. Lifecycle and crash-lock
    # tests use the same executable on Windows and Linux.
    # Compile the lock implementation directly so the tiny helper does not load
    # the geospatial DLL graph before short lifecycle deadlines begin.
    add_executable(ddbtest_child helpers/process_child.cpp ../src/library/buildlock.cpp)
    target_link_libraries(ddbtest_child PRIVATE plog::plog Threads::Threads)
    target_include_directories(ddbtest_child PRIVATE ${CUSTOM_INCLUDE_DIRS} "${PROJECT_SOURCE_DIR}/src/include")

    # Host-process stand-in for DDB-11 B6: checks that DDBRegisterProcess leaves the
    # host's signal handlers alone. Separate process: libddb initializes once per process.
    add_executable(ddbtest_signal_probe helpers/signal_probe.cpp)
    target_link_libraries(ddbtest_signal_probe PRIVATE ${PROJECT_NAME})
    target_include_directories(ddbtest_signal_probe PRIVATE ${CUSTOM_INCLUDE_DIRS} "${PROJECT_SOURCE_DIR}/src/include")

    # Create the test executable
    add_executable(ddbtest ${TEST_SOURCES})
    ddb_msvc_release_debug_info(ddbtest)
    add_dependencies(ddbtest ddbtest_child ddbtest_signal_probe)
    target_compile_definitions(ddbtest PRIVATE DDB_TEST_CHILD_NAME="$<TARGET_FILE_NAME:ddbtest_child>"
        DDB_SIGNAL_PROBE_NAME="$<TARGET_FILE_NAME:ddbtest_signal_probe>")

    # Link the library and testing framework. "spz" is the in-tree static SPZ codec target
    # (vendor/CMakeLists.txt); its PUBLIC include dir (vendor/spz/src/cc) lets the gsplat tests
    # encode/decode .spz fixtures directly. libddb is SHARED, so no duplicate objects.
    # SQLite is a PRIVATE dependency of libddb; open_test.cpp calls its C API directly.
    target_link_libraries(ddbtest PRIVATE ${PROJECT_NAME} GTest::gtest GTest::gtest_main cpr::cpr GDAL::GDAL tinyobjloader::tinyobjloader OpenSSL::Crypto Exiv2::exiv2lib unofficial::sqlite3::sqlite3 spz)

    # set PLOG to PLOG_GLOBAL/PLOG_IMPORT to share instances across modules (and import on Windows)
    if(WIN32)
        target_compile_definitions(ddbtest PRIVATE PLOG_EXPORT)
        set_target_properties(ddbtest PROPERTIES ENABLE_EXPORTS 1)
        # win_crash_report.cpp walks the faulting thread's stack
        target_link_libraries(ddbtest PRIVATE dbghelp)
    else()
        target_compile_definitions(ddbtest PRIVATE PLOG_GLOBAL)
    endif()

    message(STATUS "TESTING: CUSTOM_INCLUDE_DIRS: ${CUSTOM_INCLUDE_DIRS}")

    target_include_directories(ddbtest PRIVATE ${CUSTOM_INCLUDE_DIRS})

    # Copy only the test executable to build directory (not the entire folder)
    # This fixes the issue where copy_directory overwrites cmake_install.cmake
    add_custom_command(TARGET ddbtest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ddbtest_child>"
            "$<TARGET_FILE_DIR:ddbtest>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ddbtest_child>"
            "${CMAKE_BINARY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ddbtest_signal_probe>"
            "$<TARGET_FILE_DIR:ddbtest>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ddbtest_signal_probe>"
            "${CMAKE_BINARY_DIR}"
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
