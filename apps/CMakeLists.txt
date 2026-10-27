set(CMD_SRC_LIST "")

add_subdirectory(cmd)

# cxxopts parses the command line; only the CLI uses it, libddb does not
find_package(cxxopts CONFIG REQUIRED)

# The commands, the registry and the handler that maps their exceptions to exit codes:
# an object library, so the unit tests (tests/unit/cli_test.cpp) link the same objects the
# CLI is made of. They use the library's headers (ddb_static_api): the CLI is linked with the
# library objects, not with libddb, which exports nothing but its C API.
add_library(ddb_cli_commands OBJECT ${CMD_SRC_LIST})
target_include_directories(ddb_cli_commands PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ddb_cli_commands PUBLIC ddb_static_api cxxopts::cxxopts PRIVATE ddb_warnings)

# crash_handlers + segvcatch are CLI-only: they replace process-wide signal handlers,
# which libddb must never do inside a host process.
add_executable(${PROJECT_NAME}cmd main.cpp crash_handlers.cpp
    ${CMAKE_SOURCE_DIR}/vendor/segvcatch/segvcatch.cpp $<TARGET_OBJECTS:ddb_cli_commands>)

# The CLI contains the library (ddb_embed_library, src/CMakeLists.txt): it uses C++ internals that
# libddb does not export. libddb stays a dependency of the target so that a build of ddbcmd also
# stages everything next to it (libddb for the hosts, the data files and, on Windows, the DLLs):
# the packages ship ddbcmd and libddb from the same folder.
ddb_embed_library(${PROJECT_NAME}cmd)
# The programs of the package run on datasets that can lie deep (cmake/LongPathAware.cmake)
ddb_long_path_aware(${PROJECT_NAME}cmd)
target_link_libraries(${PROJECT_NAME}cmd PRIVATE cxxopts::cxxopts ddb_warnings)
# ddb bm3d and the commands that read or write image metadata run the helper programs
# (scripts/helper-tools.txt), found next to the executable: built with the CLI
add_dependencies(${PROJECT_NAME}cmd ${PROJECT_NAME} ${DDB_HELPER_TOOLS})
