file(GLOB SOURCES "*.cpp")

add_executable(test ${SOURCES})
target_link_libraries(test gtest_main ${LINK_LIBRARIES})
add_test(NAME all_tests COMMAND test)

