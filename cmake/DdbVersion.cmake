# The version of libddb, of its C ABI and of the release is ONE: the "version" of package.json.
#
# ddb_read_version(<package.json>)
#     Sets APP_VERSION ("2.0.0") and DDB_VERSION_MAJOR/MINOR/PATCH in the caller's scope.
#
# ddb_generate_version_header(<package.json> <template> <output>)
#     Writes ddb_version.h (src/library/ddb_version.h.in) from the version of package.json. The
#     header is the one ddb.h includes; the ABI is its MAJOR and MINOR (scripts/check-abi.py).
#     Rewritten only when its content changes.
#
# Both the build (CMakeLists.txt) and the release job (cmake -P GenerateVersionHeader.cmake, so
# that the header published next to ddb.h is the one a build makes) use them.

macro(ddb_read_version package_json)
    file(READ "${package_json}" _ddb_package_json)
    string(REGEX MATCH "\"version\": *\"([0-9]+)\\.([0-9]+)\\.([0-9]+)\"" _ddb_version_match "${_ddb_package_json}")
    if(NOT _ddb_version_match)
        message(FATAL_ERROR "${package_json}: no \"version\": \"MAJOR.MINOR.PATCH\"")
    endif()
    set(DDB_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(DDB_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(DDB_VERSION_PATCH "${CMAKE_MATCH_3}")
    set(APP_VERSION "${DDB_VERSION_MAJOR}.${DDB_VERSION_MINOR}.${DDB_VERSION_PATCH}")
endmacro()

function(ddb_generate_version_header package_json template output)
    ddb_read_version("${package_json}")
    configure_file("${template}" "${output}" @ONLY)
endfunction()
