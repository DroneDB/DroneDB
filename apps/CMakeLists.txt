set(CMD_SRC_LIST "")

add_subdirectory(cmd)

# crash_handlers + segvcatch are CLI-only: they replace process-wide signal handlers,
# which libddb must never do inside a host process (DDB-11 B6).
add_executable(${PROJECT_NAME}cmd main.cpp crash_handlers.cpp
    ${CMAKE_SOURCE_DIR}/vendor/segvcatch/segvcatch.cpp ${CMD_SRC_LIST})

# cxxopts parses the command line; only the CLI uses it, libddb does not
find_package(cxxopts CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME}cmd PRIVATE ${PROJECT_NAME} cxxopts::cxxopts)

if(WIN32)
    target_compile_definitions(${PROJECT_NAME}cmd PRIVATE PLOG_EXPORT)
    set_target_properties(${PROJECT_NAME}cmd PROPERTIES ENABLE_EXPORTS 1)
else()
    target_compile_definitions(${PROJECT_NAME}cmd PRIVATE PLOG_GLOBAL)
endif()
