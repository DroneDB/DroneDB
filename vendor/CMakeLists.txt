# segvcatch is built into the ddb CLI only (apps/CMakeLists.txt): its handlers must
# never be installed in host processes that load libddb (DDB-11 B6).
add_subdirectory("url")
add_subdirectory("base64")
# The helper programs libddb starts as child processes: scripts/helper-tools.txt is the only list
# of them (the packaging scripts and the package check read it too). The names are checked below,
# once their subdirectories have defined the targets.
set(DDB_HELPER_TOOLS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/helper-tools.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DDB_HELPER_TOOLS_FILE}")
file(STRINGS "${DDB_HELPER_TOOLS_FILE}" _helper_lines)
set(DDB_HELPER_TOOLS "")
foreach(_line IN LISTS _helper_lines)
    string(REGEX REPLACE "^[ \t\r]+|[ \t\r]+$" "" _line "${_line}")
    if(_line STREQUAL "" OR _line MATCHES "^#")
        continue()
    endif()
    string(REGEX REPLACE "[ \t]*\\|.*$" "" _helper "${_line}")
    list(APPEND DDB_HELPER_TOOLS "${_helper}")
endforeach()
if(NOT DDB_HELPER_TOOLS)
    message(FATAL_ERROR "scripts/helper-tools.txt lists no helper program")
endif()

# bm3d is built for its bm3d program (GPL-3.0), which libddb runs as a child process;
# nothing of it is linked into libddb or the CLI
add_subdirectory("bm3d")
# metadata-util (GPL-2.0-or-later, Exiv2) is a separate program too: libddb sends it requests over
# a pipe (exif_helper_pool.h) and links none of its code. It takes APP_VERSION from here.
add_subdirectory("metadata-util")

foreach(_helper IN LISTS DDB_HELPER_TOOLS)
    if(NOT TARGET ${_helper})
        message(FATAL_ERROR "scripts/helper-tools.txt names '${_helper}', which is not a CMake target "
                            "(no vendor/ subdirectory defines it)")
    endif()
endforeach()
# Read by src/, apps/ and tests/ (add_dependencies on the helpers)
set(DDB_HELPER_TOOLS "${DDB_HELPER_TOOLS}" PARENT_SCOPE)

# SPZ - native Gaussian Splat (.spz) encoder/decoder (nianticlabs/spz, MIT).
#
# Built here as a static library under DroneDB's control instead of using SPZ's own
# CMakeLists (which would fetch its own zlib/zstd and build optional CLI tools and
# Python bindings). This keeps the integration point in one place and links the
# exact zlib/zstd targets vcpkg already provides (zlib direct, zstd via the manifest
# and pdal[zstd]). SPZ only needs zlib + zstd, so no new third-party dependency.
find_package(ZLIB REQUIRED)
find_package(zstd CONFIG REQUIRED)

add_library(spz STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/spz/src/cc/load-spz.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/spz/src/cc/splat-c-types.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/spz/src/cc/splat-types.cc"
)
target_include_directories(spz PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/spz/src/cc")
set_target_properties(spz PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
target_link_libraries(spz PRIVATE ZLIB::ZLIB)

# vcpkg exposes zstd under different target names depending on triplet/version.
if(TARGET zstd::libzstd)
    target_link_libraries(spz PRIVATE zstd::libzstd)
elseif(TARGET zstd::libzstd_static)
    target_link_libraries(spz PRIVATE zstd::libzstd_static)
elseif(TARGET zstd::libzstd_shared)
    target_link_libraries(spz PRIVATE zstd::libzstd_shared)
else()
    message(FATAL_ERROR "spz: no usable zstd target found (expected zstd::libzstd*)")
endif()

# laz-perf (Apache 2.0), the LAZ codec vendored with Untwine. PDAL keeps its own copy
# private, so libddb links this one to decompress COPC nodes itself (DDB-06: decoded
# nodes are cached across point-cloud tiles). Symbols stay hidden inside libddb.
file(GLOB LAZPERF_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/untwine/lazperf/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/untwine/lazperf/detail/*.cpp"
)
add_library(lazperf STATIC ${LAZPERF_SOURCES})
target_include_directories(lazperf PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/untwine")
set_target_properties(lazperf PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

set(SRC_LIST
    ${SRC_LIST}
    ${CMAKE_CURRENT_SOURCE_DIR}/zonedetect/library/zonedetect.c
    PARENT_SCOPE
)

# Third-party headers do not count as our warnings (cmake/Warnings.cmake)
ddb_system_includes(spz lazperf)
