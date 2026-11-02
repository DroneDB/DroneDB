# ddb_long_path_aware(<target>...)
# Declares the Windows programs long path aware: the application manifest
# (cmake/long-path-aware.manifest) is added to each executable target. A .manifest among the
# sources of an MSVC target is merged by CMake with the manifest the linker makes and embedded in
# the program (Visual Studio generators through the manifest tool, Ninja through mt.exe), so the
# default manifest content is kept.
#
# The setting belongs to the process, not to a library: the manifest of the executable that
# starts the process decides, and a manifest in a DLL such as ddb.dll counts for nothing. Every
# program of the package that opens the files of a dataset (ddbcmd, the helper programs, Untwine)
# and every test binary needs it. It does not change the code that fixes the length of a path
# (a MAX_PATH buffer), and where Windows has long paths off (LongPathsEnabled) it does nothing.
set(DDB_LONG_PATH_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/long-path-aware.manifest")

function(ddb_long_path_aware)
    if(MSVC)
        foreach(target IN LISTS ARGN)
            target_sources(${target} PRIVATE "${DDB_LONG_PATH_MANIFEST}")
        endforeach()
    endif()
endfunction()
