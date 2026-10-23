set(CMD_SRC_LIST "")

add_subdirectory(cmd)

# cxxopts parses the command line; only the CLI uses it, libddb does not
find_package(cxxopts CONFIG REQUIRED)

# The commands, the registry and the handler that maps their exceptions to exit codes (DQ-12):
# an object library, so the unit tests (tests/unit/dq12_cli_test.cpp) link the same objects the
# CLI is made of. They reach libddb only through its public headers.
add_library(ddb_cli_commands OBJECT ${CMD_SRC_LIST})
target_include_directories(ddb_cli_commands PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ddb_cli_commands PUBLIC ddb_headers cxxopts::cxxopts PRIVATE ddb_warnings)

# crash_handlers + segvcatch are CLI-only: they replace process-wide signal handlers,
# which libddb must never do inside a host process (DDB-11 B6).
add_executable(${PROJECT_NAME}cmd main.cpp crash_handlers.cpp
    ${CMAKE_SOURCE_DIR}/vendor/segvcatch/segvcatch.cpp $<TARGET_OBJECTS:ddb_cli_commands>)

target_link_libraries(${PROJECT_NAME}cmd PRIVATE ${PROJECT_NAME} cxxopts::cxxopts ddb_warnings)

if(WIN32)
    target_compile_definitions(${PROJECT_NAME}cmd PRIVATE PLOG_EXPORT)
    set_target_properties(${PROJECT_NAME}cmd PROPERTIES ENABLE_EXPORTS 1)
else()
    target_compile_definitions(${PROJECT_NAME}cmd PRIVATE PLOG_GLOBAL)
endif()
