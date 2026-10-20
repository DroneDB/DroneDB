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

    file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")

    # Keep the child helper's main outside TEST_SOURCES. Lifecycle and crash-lock
    # tests use the same executable on Windows and Linux.
    # Compile the lock implementation directly so the tiny helper does not load
    # the geospatial DLL graph before short lifecycle deadlines begin.
    add_executable(ddbtest_child helpers/process_child.cpp ../src/library/buildlock.cpp)
    target_link_libraries(ddbtest_child PRIVATE plog::plog Threads::Threads)
    target_include_directories(ddbtest_child PRIVATE "${PROJECT_SOURCE_DIR}/src/include")

    # Host-process stand-in for DDB-11 B6: checks that DDBRegisterProcess leaves the
    # host's signal handlers alone. Separate process: libddb initializes once per process.
    add_executable(ddbtest_signal_probe helpers/signal_probe.cpp)
    target_link_libraries(ddbtest_signal_probe PRIVATE ${PROJECT_NAME})

    # Create the test executable
    add_executable(ddbtest ${TEST_SOURCES})
    ddb_msvc_release_debug_info(ddbtest)
    add_dependencies(ddbtest ddbtest_child ddbtest_signal_probe)
    target_compile_definitions(ddbtest PRIVATE DDB_TEST_CHILD_NAME="$<TARGET_FILE_NAME:ddbtest_child>"
        DDB_SIGNAL_PROBE_NAME="$<TARGET_FILE_NAME:ddbtest_signal_probe>")
    # The version test compares ddb::version() with package.json; one source file only, so
    # no other test TU carries it on its compile line
    set_source_files_properties(dq01_version_test.cpp PROPERTIES
        COMPILE_DEFINITIONS DDB_PACKAGE_JSON="${PROJECT_SOURCE_DIR}/package.json")

    # Link the library and testing framework. "spz" is the in-tree static SPZ codec target
    # (vendor/CMakeLists.txt); its PUBLIC include dir (vendor/spz/src/cc) lets the gsplat tests
    # encode/decode .spz fixtures directly. libddb is SHARED, so no duplicate objects.
    # SQLite is a PRIVATE dependency of libddb; open_test.cpp calls its C API directly, and
    # utils.cpp uses boolinq (header-only INTERFACE target, src/CMakeLists.txt).
    target_link_libraries(ddbtest PRIVATE ${PROJECT_NAME} GTest::gtest GTest::gtest_main cpr::cpr GDAL::GDAL tinyobjloader::tinyobjloader OpenSSL::Crypto Exiv2::exiv2lib unofficial::sqlite3::sqlite3 spz boolinq)

    # set PLOG to PLOG_GLOBAL/PLOG_IMPORT to share instances across modules (and import on Windows)
    if(WIN32)
        target_compile_definitions(ddbtest PRIVATE PLOG_EXPORT)
        set_target_properties(ddbtest PROPERTIES ENABLE_EXPORTS 1)
        # win_crash_report.cpp walks the faulting thread's stack
        target_link_libraries(ddbtest PRIVATE dbghelp)
    else()
        target_compile_definitions(ddbtest PRIVATE PLOG_GLOBAL)
    endif()

    # ddbtest, ddbtest_child and ddbtest_signal_probe are built in DDB_RUNTIME_DIR (root
    # CMakeLists.txt), next to libddb and the data files. On Windows, also stage the test's DLLs.
    if (WIN32)
        add_custom_command(TARGET ddbtest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Staging runtime DLLs for ddbtest..."
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:ddbtest>
                "${DDB_RUNTIME_DIR}"
            COMMAND_EXPAND_LISTS
        )
    endif()

    # CLI exit codes (DQ-02): usage errors and failed commands must exit 1, an explicit
    # help request exits 0. Each test checks the exact code (cli_expect_exit.cmake), so a
    # crash or a loader failure no longer counts as an expected failure. ddbcmd is built in
    # the build root, next to the Windows DLLs: the binary users run.
    set(DDB_CLI_EXE "$<TARGET_FILE:${PROJECT_NAME}cmd>")
    function(ddb_add_cli_test)
        cmake_parse_arguments(CLI "" "NAME;EXIT;WORKING_DIRECTORY" "ARGS" ${ARGN})
        if(NOT CLI_WORKING_DIRECTORY)
            set(CLI_WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        endif()
        add_test(NAME ${CLI_NAME}
            COMMAND ${CMAKE_COMMAND} -DEXPECTED=${CLI_EXIT}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_expect_exit.cmake" -- "${DDB_CLI_EXE}" ${CLI_ARGS}
            WORKING_DIRECTORY "${CLI_WORKING_DIRECTORY}")
    endfunction()

    set(DDB_CLI_EMPTY_DIR "${CMAKE_CURRENT_BINARY_DIR}/cli_not_a_dataset")
    file(MAKE_DIRECTORY "${DDB_CLI_EMPTY_DIR}")
    ddb_add_cli_test(NAME cli.NoArgsFails EXIT 1)
    ddb_add_cli_test(NAME cli.UnknownCommandFails EXIT 1 ARGS nosuchcommand)
    ddb_add_cli_test(NAME cli.PushOutsideDatasetFails EXIT 1 ARGS push WORKING_DIRECTORY "${DDB_CLI_EMPTY_DIR}")
    ddb_add_cli_test(NAME cli.InvalidArgsFail EXIT 1 ARGS cog)
    ddb_add_cli_test(NAME cli.HelpSucceeds EXIT 0 ARGS --help)
    ddb_add_cli_test(NAME cli.CommandHelpSucceeds EXIT 0 ARGS cog --help)

    # Commands that need a dataset without a remote tag: push/pull used to print the error
    # on stdout and exit 0, an unknown password subcommand printed nothing and exited 0
    set(DDB_CLI_DATASET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cli_untagged_dataset")
    file(MAKE_DIRECTORY "${DDB_CLI_DATASET_DIR}")
    add_test(NAME cli.setup.CleanDataset COMMAND ${CMAKE_COMMAND} -E rm -rf "${DDB_CLI_DATASET_DIR}/.ddb")
    ddb_add_cli_test(NAME cli.setup.InitDataset EXIT 0 ARGS init "${DDB_CLI_DATASET_DIR}")
    set_tests_properties(cli.setup.CleanDataset PROPERTIES FIXTURES_SETUP cli_clean)
    set_tests_properties(cli.setup.InitDataset PROPERTIES FIXTURES_REQUIRED cli_clean FIXTURES_SETUP cli_dataset)
    ddb_add_cli_test(NAME cli.PushWithoutTagFails EXIT 1 ARGS push WORKING_DIRECTORY "${DDB_CLI_DATASET_DIR}")
    ddb_add_cli_test(NAME cli.PullWithoutTagFails EXIT 1 ARGS pull WORKING_DIRECTORY "${DDB_CLI_DATASET_DIR}")
    ddb_add_cli_test(NAME cli.PasswordUnknownSubcommandFails EXIT 1 ARGS password nosuchsubcommand
        WORKING_DIRECTORY "${DDB_CLI_DATASET_DIR}")
    set_tests_properties(cli.PushWithoutTagFails cli.PullWithoutTagFails cli.PasswordUnknownSubcommandFails
        PROPERTIES FIXTURES_REQUIRED cli_dataset)

    include(GoogleTest)
    gtest_discover_tests(ddbtest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DISCOVERY_TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
    )

endif()
