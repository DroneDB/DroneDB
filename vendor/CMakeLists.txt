# segvcatch is built into the ddb CLI only (apps/CMakeLists.txt): its handlers must
# never be installed in host processes that load libddb (DDB-11 B6).
add_subdirectory("url")
add_subdirectory("base64")
add_subdirectory("libnexus")

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
ddb_system_includes(nxs nexus vcglib corto spz lazperf)
