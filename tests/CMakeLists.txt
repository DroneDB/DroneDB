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
    find_package(cxxopts CONFIG REQUIRED)

    # Two test binaries (DQ-03), so the fast offline tests run on their own (and under the
    # sanitizers) and the rest keeps the downloads, the external tools and the 3D builds apart.
    # Both contain the library (DQ-14): libddb exports nothing but its C API, and the tests use the
    # C++ code, so they link the library objects (ddb_embed_library, src/CMakeLists.txt) like the
    # CLI. The shared libddb is tested as it ships by the probes below (a process each, C API
    # only) and by scripts/check-abi.py exports (the dynamic symbols equal the manifest).
    #
    #   ddb_unit_tests  tests/unit/: self-contained tests. A test file belongs here when none of
    #                   its tests downloads anything (TestFS or downloadTestAsset with a URL),
    #                   calls a network service (DSM elevation), runs an external tool
    #                   (Obj2Tiles, untwine, dwg2dxf, build-lod; the fake tools a test writes
    #                   itself, e.g. ddbtest_child, are fine), reads CAD assets or builds a 3D
    #                   model (Nexus/3D Tiles: libnexus and Obj2Tiles). Whole files are
    #                   classified, so a file with one such test is an integration file.
    #                   main.cpp initializes the process through the core
    #                   (ddb::registerProcess); ddb_capi is there for the unit tests of the C
    #                   API functions (DDBGet, DDBList, DDBCalculateVolume, ...); the CLI's
    #                   commands (ddb_cli_commands, apps/CMakeLists.txt) are linked for the unit
    #                   tests of the command registry and of the handler that maps exceptions to
    #                   exit codes.
    #   ddbtest         tests/integration/: every other test. The name is kept (CI, packaging
    #                   docs, the Windows crash-dump key).
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

    # DQ-14 probe: libddb as a host loads it (C API only), the one test that runs the shared library
    # itself. ctest runs it (shared.LibddbSmoke below).
    add_executable(ddbtest_shared_probe helpers/shared_probe.cpp)
    target_link_libraries(ddbtest_shared_probe PRIVATE ${PROJECT_NAME} ddb_warnings)

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
    target_link_libraries(ddb_test_support PUBLIC ddb_static_api GTest::gtest cpr::cpr GDAL::GDAL boolinq
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
    # The ABI test compares DDBGetCapabilities() with the symbol manifest (src/CMakeLists.txt),
    # and the ABI macros (generated from package.json) with package.json
    set_source_files_properties(unit/abi_test.cpp PROPERTIES
        COMPILE_DEFINITIONS DDB_SYMBOLS_MANIFEST="${DDB_SYMBOLS_MANIFEST}")
    set_property(SOURCE unit/abi_test.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS DDB_PACKAGE_JSON="${PROJECT_SOURCE_DIR}/package.json")
    # The golden outputs of the geo and raster code (DQ-06): read from the source tree, and
    # rewritten there with DDB_UPDATE_DQ06_GOLDEN=1 (see dq06_golden_support.h)
    set_source_files_properties(dq06_golden_support.cpp PROPERTIES
        COMPILE_DEFINITIONS DDB_DQ06_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/dq06_golden")
    # The initIndex race test, the process teardown test, the GDAL data test and the test area test run their own binary as child processes
    set_source_files_properties(unit/init_index_template_test.cpp unit/process_finalize_test.cpp unit/data_dirs_test.cpp
        unit/testarea_isolation_test.cpp PROPERTIES
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
        # tests/ before the library headers, so that a test helper header (tests/utils.h) is never
        # shadowed by a library header of the same name
        target_include_directories(${name} BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        ddb_embed_library(${name})
        target_link_libraries(${name} PRIVATE ${T_LINK} ddb_test_support ddb_warnings)
        add_dependencies(${name} ddbtest_child ddbtest_signal_probe ddbtest_config_probe ddbtest_log_probe
            ddbtest_nexus_probe ddbtest_shared_probe)

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
    # flag or define of the C++ targets reaches it, but DDB_STATIC_DEFINE: it is linked with the
    # library objects (ddb_export.h), as the unit tests are.
    add_library(ddb_c11_header OBJECT unit/c11_header.c)
    set_target_properties(ddb_c11_header PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_definitions(ddb_c11_header PRIVATE DDB_STATIC_DEFINE)
    target_include_directories(ddb_c11_header PRIVATE "${PROJECT_SOURCE_DIR}/src/include"
                                                      "${DDB_GENERATED_INCLUDE_DIR}")
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
        SOURCES ${DDB_UNIT_TEST_SOURCES} $<TARGET_OBJECTS:ddb_c11_header>
        LINK ddb_cli_commands
        SERIAL DDB03_Delta.isLinear
    )
    # The C API's private headers (src/library/c_api_util.h), for the unit tests of its mapper;
    # after tests/ and the public headers, so no name there is shadowed
    target_include_directories(ddb_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/library")
    # Same runtime files as libddb (data files, Windows DLLs): the unit binary does not depend
    # on the ddb target
    ddb_stage_runtime_files(ddb_unit_tests)

    # The same library objects and dependencies as the unit tests (COMMON_LIBS links pdalcpp
    # before the static GDAL archive: one GDAL instance in the process), plus the data files:
    # the integration binary does not depend on the ddb target either
    ddb_add_test_binary(ddbtest
        LABEL integration
        TIMEOUT 900
        SOURCES ${DDB_INTEGRATION_TEST_SOURCES}
        SLOW thumbnail.orthoEdgeBatch EdgeRasterTest.NodataInfinity52Bands
    )
    ddb_stage_runtime_files(ddbtest)

    # Every header compiles on its own (DQ-14): one tiny translation unit per header (a single
    # #include), so a header that needs another one it does not include fails here and not in the
    # next file that happens to include it first. Three object libraries, built on request
    # (ctest -L headers, or the ddb_header_check target) because they compile every header again:
    #   * the headers libddb installs (ddb.h, ddb_version.h, ddb_export.h; CMakeLists.txt), as C++
    #     and as C11, with warnings as errors, in a folder that holds copies of those files and
    #     nothing else: what a user of the installed library sees. Not DDB_STATIC_DEFINE: a user
    #     links libddb;
    #   * every other header of src/include (the C++ API of the library objects), as C++, with the
    #     flags and include directories of the library sources.
    set(DDB_HEADER_CHECK_DIR "${CMAKE_CURRENT_BINARY_DIR}/header_check")
    set(DDB_INSTALLED_HEADERS ddb.h ddb_version.h ddb_export.h)
    file(MAKE_DIRECTORY "${DDB_HEADER_CHECK_DIR}/installed")
    configure_file("${PROJECT_SOURCE_DIR}/src/include/ddb.h" "${DDB_HEADER_CHECK_DIR}/installed/ddb.h" COPYONLY)
    configure_file("${DDB_GENERATED_INCLUDE_DIR}/ddb_version.h" "${DDB_HEADER_CHECK_DIR}/installed/ddb_version.h" COPYONLY)
    configure_file("${PROJECT_SOURCE_DIR}/src/include/ddb_export.h" "${DDB_HEADER_CHECK_DIR}/installed/ddb_export.h" COPYONLY)

    # ddb_header_units(<out> <language extension> <folder> <header>...): writes the units of the
    # headers, sets <out> to their paths. The typedef is there because a C translation unit with
    # nothing but macros is empty, which -Wpedantic rejects.
    function(ddb_header_units out extension folder)
        set(units "")
        foreach(header IN LISTS ARGN)
            set(unit "${DDB_HEADER_CHECK_DIR}/${folder}/${header}.${extension}")
            file(CONFIGURE OUTPUT "${unit}" CONTENT "#include \"${header}\"\ntypedef int ddb_header_unit;\n" @ONLY)
            list(APPEND units "${unit}")
        endforeach()
        set(${out} "${units}" PARENT_SCOPE)
    endfunction()

    ddb_header_units(DDB_INSTALLED_UNITS_CXX cpp installed_cxx ${DDB_INSTALLED_HEADERS})
    ddb_header_units(DDB_INSTALLED_UNITS_C c installed_c ${DDB_INSTALLED_HEADERS})
    add_library(ddb_installed_headers_cxx OBJECT EXCLUDE_FROM_ALL ${DDB_INSTALLED_UNITS_CXX})
    add_library(ddb_installed_headers_c OBJECT EXCLUDE_FROM_ALL ${DDB_INSTALLED_UNITS_C})
    set_target_properties(ddb_installed_headers_c PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    foreach(target ddb_installed_headers_cxx ddb_installed_headers_c)
        target_include_directories(${target} PRIVATE "${DDB_HEADER_CHECK_DIR}/installed")
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /WX)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endforeach()

    file(GLOB DDB_INTERNAL_HEADERS CONFIGURE_DEPENDS RELATIVE "${PROJECT_SOURCE_DIR}/src/include"
        "${PROJECT_SOURCE_DIR}/src/include/*.h")
    list(REMOVE_ITEM DDB_INTERNAL_HEADERS ${DDB_INSTALLED_HEADERS})
    ddb_header_units(DDB_INTERNAL_UNITS cpp internal ${DDB_INTERNAL_HEADERS})
    ddb_add_library_objects(ddb_internal_headers ${DDB_INTERNAL_UNITS})
    set_target_properties(ddb_internal_headers PROPERTIES EXCLUDE_FROM_ALL ON)

    add_custom_target(ddb_header_check DEPENDS ddb_installed_headers_cxx ddb_installed_headers_c ddb_internal_headers)
    set_target_properties(ddb_header_check PROPERTIES EXCLUDE_FROM_ALL ON)
    add_test(NAME headers.CompileOnTheirOwn
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target ddb_header_check --parallel 8)
    set_tests_properties(headers.CompileOnTheirOwn PROPERTIES LABELS headers TIMEOUT 1800)

    # libddb as it ships: the dataset flow of a host through the C API, in the probe's own process
    add_test(NAME shared.LibddbSmoke COMMAND ddbtest_shared_probe WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    # (label "shared", not "unit": the sanitizer job builds the unit tests and the CLI only)
    set_tests_properties(shared.LibddbSmoke PROPERTIES LABELS shared TIMEOUT 300
        ENVIRONMENT "DDB_PROBE_INPUT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/contract/inputs;DDB_PROBE_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/shared_probe_dataset")

    # Every export of ddb.h is called by a test (DQ-03): a new export without one fails here
    add_test(NAME c_abi.ExportsHaveTests
        COMMAND ${CMAKE_COMMAND} "-DDDB_HEADER=${PROJECT_SOURCE_DIR}/src/include/ddb.h"
            "-DDDB_TESTS=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/c_abi_coverage.cmake")
    set_tests_properties(c_abi.ExportsHaveTests PROPERTIES LABELS unit TIMEOUT 60)

    # scripts/check-abi.py enforces semver between the exports and package.json: its rules have
    # a test of their own, on throw-away git repositories (Python and git are the CI's already)
    find_package(Python3 COMPONENTS Interpreter)
    find_program(GIT_EXECUTABLE git)
    if(Python3_Interpreter_FOUND AND GIT_EXECUTABLE)
        add_test(NAME abi.CheckAbiSemver
            COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_abi_test.py")
        set_tests_properties(abi.CheckAbiSemver PROPERTIES LABELS unit TIMEOUT 120)
    endif()

    # CLI exit codes (DQ-02): usage errors and failed commands must exit 1, an explicit
    # help request exits 0. Each test checks the exact code (cli_expect_exit.cmake), so a
    # crash or a loader failure no longer counts as an expected failure. ddbcmd is built in
    # the build root, next to the Windows DLLs: the binary users run.
    set(DDB_CLI_EXE "$<TARGET_FILE:${PROJECT_NAME}cmd>")
    function(ddb_add_cli_test)
        cmake_parse_arguments(CLI "JSON_STDOUT;EMPTY_STDOUT" "NAME;EXIT;WORKING_DIRECTORY;STDOUT;STDERR" "ARGS" ${ARGN})
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
        if(CLI_EMPTY_STDOUT)
            list(APPEND CLI_CHECK_ARGS "-DEXPECTED_EMPTY_STDOUT=ON")
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

    # Errors and usage go to stderr and leave stdout empty (DQ-12: one handler maps what a command
    # throws to stderr and an exit code); an alias runs its command
    # contour could not run, nor print its help: the default of --min and --max was the text "nan"
    ddb_add_cli_test(NAME cli.ContourHelpSucceeds EXIT 0 STDOUT "--interval" ARGS contour --help)
    ddb_add_cli_test(NAME cli.UnknownCommandIsReportedOnStderr EXIT 1 EMPTY_STDOUT STDERR "Unknown command: nosuchcommand"
        ARGS nosuchcommand)
    ddb_add_cli_test(NAME cli.NoArgsPrintsTheCommandsOnStderr EXIT 1 EMPTY_STDOUT STDERR "Commands:")
    ddb_add_cli_test(NAME cli.MissingArgumentIsAUsageErrorOnStderr EXIT 1 EMPTY_STDOUT STDERR "Usage:" ARGS cog)
    ddb_add_cli_test(NAME cli.MissingArgumentNamesTheArgument EXIT 1 EMPTY_STDOUT
        STDERR "error: missing required argument 'input'" ARGS info)
    ddb_add_cli_test(NAME cli.UnknownOptionIsAUsageError EXIT 1 EMPTY_STDOUT STDERR "error: " ARGS cog --no-such-option)
    ddb_add_cli_test(NAME cli.AliasRunsItsCommand EXIT 0 STDOUT "List indexed files" ARGS ls --help)
    ddb_add_cli_test(NAME cli.RemovedAliasIsUnknown EXIT 1 STDERR "Unknown command: attr" ARGS attr --help)

    # The log goes to stderr: stdout carries only the output of the command. --debug makes
    # every run log (process start-up, GDAL/PROJ set-up), so a record on stdout breaks the JSON.
    set(DDB_CLI_INFO_FILE "${CMAKE_CURRENT_BINARY_DIR}/cli_info.txt")
    file(WRITE "${DDB_CLI_INFO_FILE}" "alpha")
    ddb_add_cli_test(NAME cli.InfoJsonStdoutIsOnlyJson EXIT 0 JSON_STDOUT STDERR "DEBUG"
        ARGS info -f json --debug "${DDB_CLI_INFO_FILE}")

    ddb_add_cli_test(NAME cli.InvalidFormatIsAUsageError EXIT 1 EMPTY_STDOUT STDERR "error: "
        ARGS info -f xml "${DDB_CLI_INFO_FILE}")
    ddb_add_cli_test(NAME cli.SetExifOnATextFileFails EXIT 1 ARGS setexif --gps-lat 1 "${DDB_CLI_INFO_FILE}")

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
