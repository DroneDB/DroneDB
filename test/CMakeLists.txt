file(GLOB TEST_SOURCES "*.cpp")

include_directories("${CMAKE_CURRENT_SOURCE_DIR}/../vendor")
add_executable(test ${TEST_SOURCES} ${SRC_LIST})
target_link_libraries(test gtest_main ${LINK_LIBRARIES})

file(GLOB DATA_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/data"
)
file(COPY ${DATA_FILES} DESTINATION "${CMAKE_BINARY_DIR}/test")

set_target_properties(test PROPERTIES
   CXX_STANDARD 17
   )
