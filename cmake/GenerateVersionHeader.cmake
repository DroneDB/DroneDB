# Script mode: writes ddb_version.h without configuring the project (the release job):
#   cmake -DDDB_SOURCE_DIR=<repository> -DDDB_OUTPUT=<ddb_version.h> -P cmake/GenerateVersionHeader.cmake
if(NOT DDB_SOURCE_DIR OR NOT DDB_OUTPUT)
    message(FATAL_ERROR "Usage: cmake -DDDB_SOURCE_DIR=<repository> -DDDB_OUTPUT=<ddb_version.h> -P GenerateVersionHeader.cmake")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/DdbVersion.cmake")
ddb_generate_version_header("${DDB_SOURCE_DIR}/package.json"
                            "${DDB_SOURCE_DIR}/src/library/ddb_version.h.in" "${DDB_OUTPUT}")
