add_subdirectory("exiv2")
add_subdirectory("zonedetect")
add_subdirectory("cctz")
add_subdirectory("hash-library")
add_subdirectory("googletest")
add_subdirectory("base64")
add_subdirectory("url")

file(GLOB UNTWINE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/untwine/epf/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/untwine/bu/*.cpp
)
set(UNTWINE_SRCS ${UNTWINE_SRCS} ${CMAKE_CURRENT_SOURCE_DIR}/untwine/untwine/MapFile.cpp)
set(UNTWINE_SRCS ${UNTWINE_SRCS} ${CMAKE_CURRENT_SOURCE_DIR}/untwine/untwine/ProgressWriter.cpp)
set(UNTWINE_SRCS ${UNTWINE_SRCS} ${CMAKE_CURRENT_SOURCE_DIR}/untwine/untwine/ThreadPool.cpp)

if(WIN32)
set_property(
   SOURCE UNTWINE_SRCS
   PROPERTY COMPILE_DEFINITIONS
     _WINSOCKAPI_=1
)
endif()

set(SRC_LIST
   ${SRC_LIST}
   ${UNTWINE_SRCS}
   PARENT_SCOPE
)

