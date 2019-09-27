file(GLOB TEST_SOURCES "*.cpp")

include_directories("${CMAKE_CURRENT_SOURCE_DIR}/../vendor")
add_executable(test ${TEST_SOURCES} ${SRC_LIST})
target_link_libraries(test gtest_main ${LINK_LIBRARIES})

set_target_properties(test PROPERTIES
   CXX_STANDARD 17
   )
