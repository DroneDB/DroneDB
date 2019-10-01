add_subdirectory("zonedetect")
add_subdirectory("cctz")
add_subdirectory("hash-library")
add_subdirectory("googletest")

include(${CMAKE_ROOT}/Modules/ExternalProject.cmake)
ExternalProject_Add(
    GeographicLib
    GIT_REPOSITORY    https://github.com/uav4geo/GeographicLib
    GIT_TAG           master
    CONFIGURE_COMMAND cd ${CMAKE_BINARY_DIR}/vendor/GeographicLib-prefix/src/GeographicLib && cmake -D CMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/install .
    BUILD_COMMAND cd ${CMAKE_BINARY_DIR}/vendor/GeographicLib-prefix/src/GeographicLib && make -j
    INSTALL_COMMAND cd ${CMAKE_BINARY_DIR}/vendor/GeographicLib-prefix/src/GeographicLib && make install
)

set(SRC_LIST
   ${SRC_LIST}
   PARENT_SCOPE
)

