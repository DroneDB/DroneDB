# Sanitizer and coverage builds. All options are OFF by default; see docs/sanitizers.md.
#
#   DDB_ENABLE_ASAN      AddressSanitizer (+ LeakSanitizer on Linux)
#   DDB_ENABLE_UBSAN     UndefinedBehaviorSanitizer, combinable with ASan
#   DDB_ENABLE_TSAN      ThreadSanitizer, cannot be combined with ASan
#   DDB_ENABLE_COVERAGE  gcov/llvm-cov instrumentation (--coverage)
#
# Scope: the flags are directory-wide compile and link options, so include this module in the
# root CMakeLists.txt before add_subdirectory(vendor). They then reach every target compiled
# from source in this tree: ddb, ddbcmd, ddbtest and its helpers, and the vendored code built
# here (spz, lazperf, zonedetect, url, base64, libnexus with nexus/corto/vcglib). The vendored
# code is instrumented on purpose:
#   * spz, lazperf and zonedetect are linked into libddb, so their memory is libddb's memory,
#     and they decode untrusted input (.spz, LAZ, timezone data);
#   * libnexus is not part of libddb: ddb-nexus runs as a child process of the Nexus build, so the
#     sanitizer runtime of the child checks the Nexus code there (and the environment, with its
#     suppression files, is inherited from the test);
#   * the cost is nil: we compile these sources anyway.
# vcpkg dependencies (GDAL, PDAL, PROJ, SQLite, ...) are prebuilt with the triplet's flags
# and stay uninstrumented. ASan/UBSan still check the calls into them; TSan may report races
# whose frames are all inside them: tests/tsan.supp suppresses those by third-party symbol only.
#
# Reports are diagnosed and fixed in our code; suppression files (tests/lsan.supp,
# tests/tsan.supp) are for third-party frames only.

option(DDB_ENABLE_ASAN "Build with AddressSanitizer (and LeakSanitizer on Linux)" OFF)
option(DDB_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(DDB_ENABLE_TSAN "Build with ThreadSanitizer (not with DDB_ENABLE_ASAN)" OFF)
option(DDB_ENABLE_COVERAGE "Build with coverage instrumentation (--coverage)" OFF)

if(DDB_ENABLE_ASAN AND DDB_ENABLE_TSAN)
    message(FATAL_ERROR "DDB_ENABLE_ASAN and DDB_ENABLE_TSAN cannot be combined: "
        "use a separate build directory for each")
endif()

set(_ddb_sanitizers "")
if(DDB_ENABLE_ASAN)
    list(APPEND _ddb_sanitizers address)
endif()
if(DDB_ENABLE_UBSAN)
    list(APPEND _ddb_sanitizers undefined)
endif()
if(DDB_ENABLE_TSAN)
    list(APPEND _ddb_sanitizers thread)
endif()

if(MSVC)
    # MSVC has AddressSanitizer only, and no gcov-style coverage switch
    if(DDB_ENABLE_UBSAN OR DDB_ENABLE_TSAN OR DDB_ENABLE_COVERAGE)
        message(FATAL_ERROR "MSVC supports DDB_ENABLE_ASAN only")
    endif()
    if(DDB_ENABLE_ASAN)
        add_compile_options(/fsanitize=address)
        message(STATUS "Sanitizers: address")
    endif()
elseif(_ddb_sanitizers OR DDB_ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizer/coverage options need GCC or Clang (found ${CMAKE_CXX_COMPILER_ID})")
    endif()

    if(_ddb_sanitizers)
        list(JOIN _ddb_sanitizers "," _ddb_sanitize_list)
        # -fno-omit-frame-pointer: usable stacks from the fast unwinder. UBSan reports stay
        # recoverable at compile time; UBSAN_OPTIONS=halt_on_error=1 makes them fatal at run time.
        add_compile_options(-fsanitize=${_ddb_sanitize_list} -fno-omit-frame-pointer)
        add_link_options(-fsanitize=${_ddb_sanitize_list})
        message(STATUS "Sanitizers: ${_ddb_sanitize_list}")
    endif()

    if(DDB_ENABLE_COVERAGE)
        add_compile_options(--coverage)
        add_link_options(--coverage)
        message(STATUS "Coverage instrumentation: on")
    endif()
endif()
