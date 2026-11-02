# CMAKE_PROJECT_<name>_INCLUDE file for a program that is built in a tree of its own, whose
# CMakeLists.txt is not ours (Untwine, scripts/build-untwine.ps1): makes the executable targets
# named in DDB_LONG_PATH_AWARE_TARGETS long path aware (cmake/LongPathAware.cmake). CMake reads this
# file right after the project() call, before the targets exist, so the call is deferred to the end
# of the top-level directory, where they do.
include("${CMAKE_CURRENT_LIST_DIR}/LongPathAware.cmake")
cmake_language(DEFER CALL ddb_long_path_aware ${DDB_LONG_PATH_AWARE_TARGETS})
