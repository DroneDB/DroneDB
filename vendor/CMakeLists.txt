add_subdirectory("segvcatch")
add_subdirectory("url")
add_subdirectory("base64")
add_subdirectory("libnexus")

file(GLOB UNTWINE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/untwine/epf/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/untwine/bu/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/untwine/lazperf/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/untwine/lazperf/detail/*.cpp
)

list(APPEND UNTWINE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/untwine/untwine/ProgressWriter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/untwine/untwine/ThreadPool.cpp
    #${CMAKE_CURRENT_SOURCE_DIR}/untwine/untwine/Common.cpp
    #${CMAKE_CURRENT_SOURCE_DIR}/untwine/untwine/Las.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/untwine/untwine/MapFile.cpp
)

set(SRC_LIST
    ${SRC_LIST}
    ${CMAKE_CURRENT_SOURCE_DIR}/zonedetect/library/zonedetect.c
    ${UNTWINE_SRCS}
    PARENT_SCOPE
)
