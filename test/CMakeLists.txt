file(GLOB TEST_SOURCES "*.cpp")

include_directories("${CMAKE_CURRENT_SOURCE_DIR}/../vendor")
add_executable(test ${TEST_SOURCES})
target_link_libraries(test gtest_main ${PROJECT_NAME} ${LINK_LIBRARIES})

# set PLOG to PLOG_GLOBAL/PLOG_IMPORT to share instances across modules (and import on Windows)
if(WIN32)
    target_compile_definitions(test PRIVATE PLOG_IMPORT)
else()
    target_compile_definitions(test PRIVATE PLOG_GLOBAL)
endif()

# Copy DLLs
if (WIN32)
add_custom_command(TARGET test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different  $<TARGET_FILE:ddb> "${CMAKE_BINARY_DIR}/ddb.dll"
    COMMENT "Created ${CMAKE_BINARY_DIR}/ddb.dll"
)
add_custom_command(TARGET test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different  $<TARGET_FILE:gtest> "${CMAKE_BINARY_DIR}/gtest.dll"
    COMMENT "Created ${CMAKE_BINARY_DIR}/gtest.dll"
)
endif()

set(EXE_EXT ".run")
if (WIN32)
    set(EXE_EXT ".exe")
endif()

add_custom_command(TARGET test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:test> ${CMAKE_BINARY_DIR}/test${EXE_EXT}
        COMMENT "Created ${CMAKE_BINARY_DIR}/test${EXE_EXT}"
    )

file(GLOB DATA_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/data"
)

file(COPY ${DATA_FILES} DESTINATION "${CMAKE_BINARY_DIR}")

set_target_properties(test PROPERTIES CXX_STANDARD 17)
