# C ABI coverage (DQ-03): every function ddb.h exports (DDB_DLL ... DDBName(...)) must be called
# by at least one test in tests/unit/ or tests/integration/, so a new export comes with a test.
# Run by ctest (c_abi.ExportsHaveTests, label unit):
#   cmake -DDDB_HEADER=<src/include/ddb.h> -DDDB_TESTS=<tests folder> -P c_abi_coverage.cmake

if(NOT DDB_HEADER OR NOT DDB_TESTS)
    message(FATAL_ERROR "Usage: cmake -DDDB_HEADER=<ddb.h> -DDDB_TESTS=<tests folder> -P c_abi_coverage.cmake")
endif()

file(READ "${DDB_HEADER}" _header)
string(REGEX MATCHALL "DDB_DLL[^;(]*[ *]DDB[A-Za-z0-9_]+[ \t]*\\(" _declarations "${_header}")
set(_exports "")
foreach(_declaration IN LISTS _declarations)
    string(REGEX REPLACE ".*[ *](DDB[A-Za-z0-9_]+)[ \t]*\\($" "\\1" _name "${_declaration}")
    list(APPEND _exports "${_name}")
endforeach()
list(REMOVE_DUPLICATES _exports)
list(LENGTH _exports _export_count)
if(_export_count EQUAL 0)
    message(FATAL_ERROR "No exports found in ${DDB_HEADER}")
endif()

set(_tests "")
foreach(_dir IN ITEMS unit integration)
    file(GLOB _files "${DDB_TESTS}/${_dir}/*.cpp")
    foreach(_file IN LISTS _files)
        file(READ "${_file}" _content)
        string(APPEND _tests "${_content}\n")
    endforeach()
endforeach()

set(_missing "")
foreach(_name IN LISTS _exports)
    # A call: the name followed by "(" (a comment that only names the function does not count)
    string(REGEX MATCH "[^A-Za-z0-9_]${_name}[ \t]*\\(" _call "${_tests}")
    if(NOT _call)
        list(APPEND _missing "${_name}")
    endif()
endforeach()

if(_missing)
    list(JOIN _missing "\n  " _list)
    message(FATAL_ERROR "ddb.h exports without a test (add one, e.g. to tests/unit/c_abi_test.cpp):\n  ${_list}")
endif()
message(STATUS "All ${_export_count} exports of ddb.h are called by a test")
