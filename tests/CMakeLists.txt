if(BUILD_TESTING)

    message(STATUS "BUILD_TESTING is ON")

    find_package(GTest CONFIG REQUIRED)
    find_package(cpr CONFIG REQUIRED)
    find_package(GDAL CONFIG REQUIRED)
    find_package(PDAL CONFIG REQUIRED)
    find_package(tinyobjloader CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(exiv2 CONFIG REQUIRED)
    find_package(Threads REQUIRED)
    find_package(plog CONFIG REQUIRED)

    # Two test binaries (DQ-03), so the fast offline tests run on their own (and under the
    # sanitizers) and the rest keeps testing libddb as it ships:
    #
    #   ddb_unit_tests  tests/unit/: self-contained tests. A test file belongs here when none of
    #                   its tests downloads anything (TestFS or downloadTestAsset with a URL),
    #                   calls a network service (DSM elevation), runs an external tool
    #                   (Obj2Tiles, untwine, dwg2dxf, build-lod; the fake tools a test writes
    #                   itself, e.g. ddbtest_child, are fine), reads CAD assets or builds a 3D
    #                   model (Nexus/3D Tiles: libnexus and Obj2Tiles). Whole files are
    #                   classified, so a file with one such test is an integration file.
    #                   Links the library objects (ddb_core + ddb_capi, src/CMakeLists.txt)
    #                   and never libddb. main.cpp initializes the process through the core
    #                   (ddb::registerProcess); ddb_capi is linked for the unit tests of the C
    #                   API functions (DDBGet, DDBList, DDBCalculateVolume, ...).
    #   ddbtest         tests/integration/: every other test; links libddb. The name is kept
    #                   (CI, packaging docs, the Windows crash-dump key).
    #
    # tests/*.cpp (main.cpp and the shared helpers) are compiled once, into ddb_test_support,
    # and linked into both.
    file(GLOB DDB_TEST_SUPPORT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
    file(GLOB DDB_UNIT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp")
    file(GLOB DDB_INTEGRATION_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cpp")

    # Keep the child helper's main outside the test sources. Lifecycle and crash-lock
    # tests use the same executable on Windows and Linux.
    # Compile the lock implementation directly so the tiny helper does not load
    # the geospatial DLL graph before short lifecycle deadlines begin.
    add_executable(ddbtest_child helpers/process_child.cpp ../src/library/buildlock.cpp
        ../src/library/path_encoding.cpp)
    target_link_libraries(ddbtest_child PRIVATE plog::plog Threads::Threads ddb_warnings)
    target_include_directories(ddbtest_child PRIVATE "${PROJECT_SOURCE_DIR}/src/include")

    # Host-process stand-in for DDB-11 B6: checks that DDBRegisterProcess leaves the
    # host's signal handlers alone. Separate process: libddb initializes once per process.
    add_executable(ddbtest_signal_probe helpers/signal_probe.cpp)
    target_link_libraries(ddbtest_signal_probe PRIVATE ${PROJECT_NAME} ddb_warnings)

    # DQ-15 probe: DDBConfigure before DDBRegisterProcess, in a process of its own for the same
    # reason (CAbiConfig.ConfigureBeforeRegisterProcess runs it).
    add_executable(ddbtest_config_probe helpers/config_probe.cpp)
    target_link_libraries(ddbtest_config_probe PRIVATE ${PROJECT_NAME} ddb_warnings)

    # DQ-15 probe: DDBSetLogCallback before DDBRegisterProcess, in a process of its own for the
    # same reason (CAbiLog.callbackBeforeRegisterProcess runs it).
    add_executable(ddbtest_log_probe helpers/log_probe.cpp)
    target_link_libraries(ddbtest_log_probe PRIVATE ${PROJECT_NAME} ddb_warnings)

    # Nexus exit probe: the Nexus build on a worker thread, then exit() (the NexusExit tests run it
    # as a process of its own: the crash it looks for happens while the process exits).
    add_executable(ddbtest_nexus_probe helpers/nexus_probe.cpp)
    target_link_libraries(ddbtest_nexus_probe PRIVATE $<LINK_ONLY:nxs> GDAL::GDAL ddb_warnings)
    target_include_directories(ddbtest_nexus_probe PRIVATE "${PROJECT_SOURCE_DIR}/vendor/libnexus")

    # Shared test code. It compiles against the library headers only (ddb_headers): each test
    # binary supplies the library itself. testfs.cpp uses cpr, raster_test_utils.cpp GDAL,
    # utils.cpp boolinq (header-only INTERFACE target, src/CMakeLists.txt).
    add_library(ddb_test_support OBJECT ${DDB_TEST_SUPPORT_SOURCES})
    ddb_msvc_release_debug_info(ddb_test_support)
    target_include_directories(ddb_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(ddb_test_support PUBLIC
        DDB_TEST_CHILD_NAME="$<TARGET_FILE_NAME:ddbtest_child>"
        DDB_SIGNAL_PROBE_NAME="$<TARGET_FILE_NAME:ddbtest_signal_probe>"
        DDB_CONFIG_PROBE_NAME="$<TARGET_FILE_NAME:ddbtest_config_probe>"
        DDB_LOG_PROBE_NAME="$<TARGET_FILE_NAME:ddbtest_log_probe>"
        DDB_NEXUS_PROBE_NAME="$<TARGET_FILE_NAME:ddbtest_nexus_probe>")
    target_link_libraries(ddb_test_support PUBLIC ddb_headers GTest::gtest cpr::cpr GDAL::GDAL boolinq
        PRIVATE ddb_warnings)
    if(WIN32)
        # win_crash_report.cpp walks the faulting thread's stack
        target_link_libraries(ddb_test_support PUBLIC dbghelp)
    endif()

    # The DroneDB/test_data commit the tests download their assets from (testDataUrl, testfs.h).
    # One file, read here and by CI (.github/workflows/c-cpp.yml), so both use the same assets.
    set(DDB_TEST_DATA_SHA_FILE "${CMAKE_CURRENT_SOURCE_DIR}/test-data.sha")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DDB_TEST_DATA_SHA_FILE}")
    file(STRINGS "${DDB_TEST_DATA_SHA_FILE}" DDB_TEST_DATA_SHA LIMIT_COUNT 1)
    string(LENGTH "${DDB_TEST_DATA_SHA}" _sha_length)
    if(NOT _sha_length EQUAL 40 OR NOT DDB_TEST_DATA_SHA MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${DDB_TEST_DATA_SHA_FILE} must hold a full commit SHA, found '${DDB_TEST_DATA_SHA}'")
    endif()
    set_source_files_properties(testfs.cpp PROPERTIES
        COMPILE_DEFINITIONS DDB_TEST_DATA_SHA="${DDB_TEST_DATA_SHA}")
    # The golden contract fixtures (XI-09, tests/contract/README.md): read from the source tree,
    # and rewritten there with DDB_UPDATE_CONTRACT_FIXTURES=1
    set_source_files_properties(contract_fixtures.cpp PROPERTIES
        COMPILE_DEFINITIONS DDB_CONTRACT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/contract")

    # The version test compares ddb::version() with package.json; one source file only, so
    # no other test TU carries it on its compile line
    set_source_files_properties(unit/dq01_version_test.cpp PROPERTIES
        COMPILE_DEFINITIONS DDB_PACKAGE_JSON="${PROJECT_SOURCE_DIR}/package.json")
    # The ABI test compares DDBGetCapabilities() with the symbol manifest (src/CMakeLists.txt)
    set_source_files_properties(unit/abi_test.cpp PROPERTIES
        COMPILE_DEFINITIONS DDB_SYMBOLS_MANIFEST="${DDB_SYMBOLS_MANIFEST}")
    # The initIndex race test and the process teardown test run their own binary as child processes
    set_source_files_properties(unit/init_index_template_test.cpp unit/process_finalize_test.cpp PROPERTIES
        COMPILE_DEFINITIONS DDB_UNIT_TESTS_NAME="$<TARGET_FILE_NAME:ddb_unit_tests>")

    include(GoogleTest)

    # ddb_add_test_binary(<name> LABEL <label> TIMEOUT <seconds> SOURCES <src>... LINK <lib>...
    #                     [SLOW <test>...] [SERIAL <test>...])
    # A gtest binary built in DDB_RUNTIME_DIR (root CMakeLists.txt), next to libddb and the data
    # files, with the shared test code, registered in ctest with gtest_discover_tests: each test
    # gets LABELS <label> and TIMEOUT <seconds>, run from the build root. The SLOW tests (over
    # about 20 s on a fast machine, measured with --gtest_print_time) also get the label "slow"
    # and a longer timeout, so "ctest -LE slow" skips them. The SERIAL tests (wall-clock
    # complexity checks) run alone (RUN_SERIAL), so a parallel "ctest -j" does not slow them.
    set(DDB_SLOW_TEST_TIMEOUT 3600)
    function(ddb_add_test_binary name)
        cmake_parse_arguments(T "" "LABEL;TIMEOUT" "SOURCES;LINK;SLOW;SERIAL" ${ARGN})
        add_executable(${name} ${T_SOURCES})
        ddb_msvc_release_debug_info(${name})
        # tests/ before the library headers: tests/utils.h has the same name as
        # src/include/utils.h, and test sources in unit/ and integration/ mean the test one
        target_include_directories(${name} BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        # LINK first: ddbtest must resolve symbols that libddb exports (cpr, spz) from libddb,
        # before a static archive of the same library is searched
        target_link_libraries(${name} PRIVATE ${T_LINK} ddb_test_support ddb_warnings)
        add_dependencies(${name} ddbtest_child ddbtest_signal_probe ddbtest_config_probe ddbtest_log_probe
            ddbtest_nexus_probe)
        if(WIN32)
            set_target_properties(${name} PROPERTIES ENABLE_EXPORTS 1)
        endif()

        gtest_discover_tests(${name}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DISCOVERY_TIMEOUT 60
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS ${T_LABEL} TIMEOUT ${T_TIMEOUT}
        )
        # Applied after the discovered tests are defined (TEST_INCLUDE_FILES are read in order,
        # and gtest_discover_tests has just added its own); ctest ignores a name that no longer
        # exists.
        set(_extra_content "")
        foreach(_test IN LISTS T_SLOW)
            string(APPEND _extra_content
                "set_tests_properties([==[${_test}]==] PROPERTIES LABELS \"${T_LABEL};slow\" TIMEOUT ${DDB_SLOW_TEST_TIMEOUT})\n")
        endforeach()
        foreach(_test IN LISTS T_SERIAL)
            string(APPEND _extra_content "set_tests_properties([==[${_test}]==] PROPERTIES RUN_SERIAL TRUE)\n")
        endforeach()
        if(_extra_content)
            set(_extra_file "${CMAKE_CURRENT_BINARY_DIR}/${name}_extra_test_properties.cmake")
            file(WRITE "${_extra_file}" "${_extra_content}")
            set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${_extra_file}")
        endif()
    endfunction()

    # ddb.h is valid C (DQ-10, XI-03): unit/c11_header.c includes it as C11 with warnings as
    # errors, and is linked into ddb_unit_tests (unit/c_header_test.cpp calls it), so a header
    # that is not valid C fails the build of the unit tests. Only the public include folder: no
    # flag or define of the C++ targets reaches it.
    add_library(ddb_c11_header OBJECT unit/c11_header.c)
    set_target_properties(ddb_c11_header PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_include_directories(ddb_c11_header PRIVATE "${PROJECT_SOURCE_DIR}/src/include")
    if(MSVC)
        target_compile_options(ddb_c11_header PRIVATE /W4 /WX)
    else()
        target_compile_options(ddb_c11_header PRIVATE -Wall -Wextra -Wpedantic -Wstrict-prototypes -Werror)
    endif()

    # The library objects and all their dependencies, linked once: one copy of every static
    # library in the process
    ddb_add_test_binary(ddb_unit_tests
        LABEL unit
        TIMEOUT 300
        SOURCES ${DDB_UNIT_TEST_SOURCES} $<TARGET_OBJECTS:ddb_core> $<TARGET_OBJECTS:ddb_capi>
            $<TARGET_OBJECTS:ddb_c11_header>
        LINK ddb_core_usage
        SERIAL DDB03_Delta.isLinear
    )
    # The C API's private headers (src/library/c_api_util.h), for the unit tests of its mapper;
    # after tests/ and the public headers, so no name there is shadowed
    target_include_directories(ddb_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/library")
    # Same runtime files as libddb (data files, Windows DLLs): the unit binary does not depend
    # on the ddb target
    ddb_stage_runtime_files(ddb_unit_tests)

    # libddb plus the third-party libraries the integration tests call directly, each taken
    # from the module that already provides it in the process, so no second static copy is
    # linked in front of it (LINK comes first on the link line):
    #   * cpr, spz: re-exported by libddb.so;
    #   * GDAL, SQLite, curl, OpenSSL: Linux libddb resolves them from the shared libpdalcpp
    #     (vcpkg links them into it, exported), so ddbtest links pdalcpp before the static GDAL
    #     archive and uses the same GDAL instance as libddb (driver registry, error handler);
    #   * Exiv2, tinyobjloader: linked into libddb.so with hidden symbols, so the few tests that
    #     call them directly get their own copy.
    # On Windows all of them are DLLs (spz is static, not exported by ddb.dll: its own copy).
    ddb_add_test_binary(ddbtest
        LABEL integration
        TIMEOUT 900
        SOURCES ${DDB_INTEGRATION_TEST_SOURCES}
        LINK ${PROJECT_NAME} pdalcpp Exiv2::exiv2lib tinyobjloader::tinyobjloader OpenSSL::Crypto spz
        SLOW thumbnail.orthoEdgeBatch EdgeRasterTest.NodataInfinity52Bands
    )
    ddb_stage_runtime_dlls(ddbtest)

    # Every export of ddb.h is called by a test (DQ-03): a new export without one fails here
    add_test(NAME c_abi.ExportsHaveTests
        COMMAND ${CMAKE_COMMAND} "-DDDB_HEADER=${PROJECT_SOURCE_DIR}/src/include/ddb.h"
            "-DDDB_TESTS=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/c_abi_coverage.cmake")
    set_tests_properties(c_abi.ExportsHaveTests PROPERTIES LABELS unit TIMEOUT 60)

    # CLI exit codes (DQ-02): usage errors and failed commands must exit 1, an explicit
    # help request exits 0. Each test checks the exact code (cli_expect_exit.cmake), so a
    # crash or a loader failure no longer counts as an expected failure. ddbcmd is built in
    # the build root, next to the Windows DLLs: the binary users run.
    set(DDB_CLI_EXE "$<TARGET_FILE:${PROJECT_NAME}cmd>")
    function(ddb_add_cli_test)
        cmake_parse_arguments(CLI "JSON_STDOUT" "NAME;EXIT;WORKING_DIRECTORY;STDOUT;STDERR" "ARGS" ${ARGN})
        if(NOT CLI_WORKING_DIRECTORY)
            set(CLI_WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        endif()
        set(CLI_CHECK_ARGS "")
        if(DEFINED CLI_STDOUT)
            list(APPEND CLI_CHECK_ARGS "-DEXPECTED_STDOUT=${CLI_STDOUT}")
        endif()
        if(DEFINED CLI_STDERR)
            list(APPEND CLI_CHECK_ARGS "-DEXPECTED_STDERR=${CLI_STDERR}")
        endif()
        if(CLI_JSON_STDOUT)
            list(APPEND CLI_CHECK_ARGS "-DEXPECTED_JSON_STDOUT=ON")
        endif()
        add_test(NAME ${CLI_NAME}
            COMMAND ${CMAKE_COMMAND} -DEXPECTED=${CLI_EXIT} ${CLI_CHECK_ARGS}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_expect_exit.cmake" -- "${DDB_CLI_EXE}" ${CLI_ARGS}
            WORKING_DIRECTORY "${CLI_WORKING_DIRECTORY}")
        set_tests_properties(${CLI_NAME} PROPERTIES LABELS cli TIMEOUT 120)
    endfunction()

    set(DDB_CLI_EMPTY_DIR "${CMAKE_CURRENT_BINARY_DIR}/cli_not_a_dataset")
    file(MAKE_DIRECTORY "${DDB_CLI_EMPTY_DIR}")
    ddb_add_cli_test(NAME cli.NoArgsFails EXIT 1)
    ddb_add_cli_test(NAME cli.UnknownCommandFails EXIT 1 ARGS nosuchcommand)
    ddb_add_cli_test(NAME cli.PushOutsideDatasetFails EXIT 1 ARGS push WORKING_DIRECTORY "${DDB_CLI_EMPTY_DIR}")
    ddb_add_cli_test(NAME cli.InvalidArgsFail EXIT 1 ARGS cog)
    ddb_add_cli_test(NAME cli.HelpSucceeds EXIT 0 ARGS --help)
    ddb_add_cli_test(NAME cli.CommandHelpSucceeds EXIT 0 ARGS cog --help)

    # The log goes to stderr: stdout carries only the output of the command. --debug makes
    # every run log (process start-up, GDAL/PROJ set-up), so a record on stdout breaks the JSON.
    set(DDB_CLI_INFO_FILE "${CMAKE_CURRENT_BINARY_DIR}/cli_info.txt")
    file(WRITE "${DDB_CLI_INFO_FILE}" "alpha")
    ddb_add_cli_test(NAME cli.InfoJsonStdoutIsOnlyJson EXIT 0 JSON_STDOUT STDERR "DEBUG"
        ARGS info -f json --debug "${DDB_CLI_INFO_FILE}")

    # Commands that need a dataset without a remote tag: push/pull used to print the error
    # on stdout and exit 0, an unknown password subcommand printed nothing and exited 0
    set(DDB_CLI_DATASET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cli_untagged_dataset")
    file(MAKE_DIRECTORY "${DDB_CLI_DATASET_DIR}")
    add_test(NAME cli.setup.CleanDataset COMMAND ${CMAKE_COMMAND} -E rm -rf "${DDB_CLI_DATASET_DIR}/.ddb")
    ddb_add_cli_test(NAME cli.setup.InitDataset EXIT 0 ARGS init "${DDB_CLI_DATASET_DIR}")
    set_tests_properties(cli.setup.CleanDataset PROPERTIES FIXTURES_SETUP cli_clean LABELS cli)
    set_tests_properties(cli.setup.InitDataset PROPERTIES FIXTURES_REQUIRED cli_clean FIXTURES_SETUP cli_dataset)
    ddb_add_cli_test(NAME cli.PushWithoutTagFails EXIT 1 ARGS push WORKING_DIRECTORY "${DDB_CLI_DATASET_DIR}")
    ddb_add_cli_test(NAME cli.PullWithoutTagFails EXIT 1 ARGS pull WORKING_DIRECTORY "${DDB_CLI_DATASET_DIR}")
    ddb_add_cli_test(NAME cli.PasswordUnknownSubcommandFails EXIT 1 ARGS password nosuchsubcommand
        WORKING_DIRECTORY "${DDB_CLI_DATASET_DIR}")
    set_tests_properties(cli.PushWithoutTagFails cli.PullWithoutTagFails cli.PasswordUnknownSubcommandFails
        PROPERTIES FIXTURES_REQUIRED cli_dataset)

    # UTF-8 paths (src/include/mio.h, "Path encoding"): a dataset under a non-ASCII folder with a
    # non-ASCII file name. On Windows the CLI reads its arguments in UTF-16 (wmain): main's argv
    # is in the ANSI code page.
    set(DDB_CLI_UTF8_DIR "${CMAKE_CURRENT_BINARY_DIR}/cli_città_ü")
    set(DDB_CLI_UTF8_FILE "ä_日.txt")
    file(MAKE_DIRECTORY "${DDB_CLI_UTF8_DIR}")
    file(WRITE "${DDB_CLI_UTF8_DIR}/${DDB_CLI_UTF8_FILE}" "alpha")
    add_test(NAME cli.setup.CleanUtf8Dataset COMMAND ${CMAKE_COMMAND} -E rm -rf "${DDB_CLI_UTF8_DIR}/.ddb")
    ddb_add_cli_test(NAME cli.setup.InitUtf8Dataset EXIT 0 ARGS init "${DDB_CLI_UTF8_DIR}")
    set_tests_properties(cli.setup.CleanUtf8Dataset PROPERTIES FIXTURES_SETUP cli_utf8_clean LABELS cli)
    set_tests_properties(cli.setup.InitUtf8Dataset PROPERTIES FIXTURES_REQUIRED cli_utf8_clean
        FIXTURES_SETUP cli_utf8_dataset)
    ddb_add_cli_test(NAME cli.Utf8Add EXIT 0 STDOUT "${DDB_CLI_UTF8_FILE}"
        ARGS add "${DDB_CLI_UTF8_DIR}/${DDB_CLI_UTF8_FILE}" WORKING_DIRECTORY "${DDB_CLI_UTF8_DIR}")
    ddb_add_cli_test(NAME cli.Utf8List EXIT 0 STDOUT "${DDB_CLI_UTF8_FILE}"
        ARGS list WORKING_DIRECTORY "${DDB_CLI_UTF8_DIR}")
    set_tests_properties(cli.Utf8Add PROPERTIES FIXTURES_REQUIRED cli_utf8_dataset
        FIXTURES_SETUP cli_utf8_added)
    set_tests_properties(cli.Utf8List PROPERTIES FIXTURES_REQUIRED "cli_utf8_dataset;cli_utf8_added")

endif()
