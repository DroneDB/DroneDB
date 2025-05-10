set(CMD_SRC_LIST "")

add_subdirectory(cmd)

add_executable(${PROJECT_NAME}cmd main.cpp ${CMD_SRC_LIST})

# Link the static library version
target_link_libraries(${PROJECT_NAME}cmd PRIVATE ${PROJECT_NAME})

message(STATUS "DDB: CUSTOM_INCLUDE_DIRS: ${CUSTOM_INCLUDE_DIRS}")

target_include_directories(${PROJECT_NAME}cmd PRIVATE ${CUSTOM_INCLUDE_DIRS})

if(WIN32)
    target_compile_definitions(${PROJECT_NAME}cmd PRIVATE PLOG_EXPORT)
    set_target_properties(${PROJECT_NAME}cmd PROPERTIES ENABLE_EXPORTS 1)
else()
    target_compile_definitions(${PROJECT_NAME}cmd PRIVATE PLOG_GLOBAL)
endif()

if (WIN32)
    # For the ddb executable:
    add_custom_command(TARGET ${PROJECT_NAME}cmd POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/apps/$<CONFIG>"
            "${CMAKE_BINARY_DIR}"
    )
else()
    # For the ddb executable:
    add_custom_command(TARGET ${PROJECT_NAME}cmd POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/apps"
            "${CMAKE_BINARY_DIR}"
    )
endif()