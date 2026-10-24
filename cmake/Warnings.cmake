# Compiler warnings for DroneDB's own code (DQ-01, decision D10).
#
# ddb_warnings is an INTERFACE target: link it PRIVATE to every target built from src/, apps/ or
# tests/, so the flags reach their sources but never a consumer of libddb.
#
# The flags are not -Werror: the warning count is a ratchet instead (scripts/check-warnings.py and
# cmake/warnings-baseline.txt), so a change may not add a warning while the existing ones are
# removed over time. Third-party headers must not count: vcpkg's imported targets are already
# -isystem, and in-tree third-party include directories are made SYSTEM with
# ddb_system_includes() below or with target_include_directories(SYSTEM).
#
# The flags apply to C++ only: the only C sources in our targets are vendored (zonedetect.c),
# and -Wnon-virtual-dtor/-Wold-style-cast are C++-only options.

add_library(ddb_warnings INTERFACE)

if(MSVC)
    target_compile_options(ddb_warnings INTERFACE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive->)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ddb_warnings INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wnon-virtual-dtor
            -Wold-style-cast
        >
    )
endif()

# Marks the usage-requirement include directories of in-tree third-party targets as SYSTEM, so
# warnings in their headers are not reported in our sources. Works on every supported CMake
# version (the SYSTEM target property needs 3.25) and on targets defined in submodules that we
# do not edit.
function(ddb_system_includes)
    foreach(_target IN LISTS ARGN)
        if(NOT TARGET ${_target})
            message(FATAL_ERROR "ddb_system_includes: ${_target} is not a target")
        endif()
        get_target_property(_dirs ${_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(_dirs)
            set_property(TARGET ${_target} APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_dirs})
        endif()
    endforeach()
endfunction()
