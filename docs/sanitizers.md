# Sanitizer and coverage builds

DroneDB can be built with AddressSanitizer (ASan, with LeakSanitizer), UndefinedBehaviorSanitizer
(UBSan), ThreadSanitizer (TSan) and coverage instrumentation. The options live in
`cmake/Sanitizers.cmake` and are all `OFF` by default:

| Option | Effect |
| --- | --- |
| `DDB_ENABLE_ASAN` | `-fsanitize=address` (LeakSanitizer included on Linux) |
| `DDB_ENABLE_UBSAN` | `-fsanitize=undefined`, combinable with ASan |
| `DDB_ENABLE_TSAN` | `-fsanitize=thread`; configuring it together with ASan is an error |
| `DDB_ENABLE_COVERAGE` | `--coverage` (gcov/llvm-cov) |

GCC and Clang support all four. MSVC supports `DDB_ENABLE_ASAN` only.

Every target compiled in this tree is instrumented: the library objects (`ddb_core`, `ddb_capi`)
and so `ddb`, `ddbcmd`, both test binaries (`ddb_unit_tests`, `ddbtest`) and their helpers, and the
vendored code built here (spz, lazperf, zonedetect, and the `metadata-util` and `bm3d` helper
executables (bm3d with the Nexus, corto and vcglib code in it), which the test targets build next
to the test binaries).
The vcpkg dependencies (GDAL, PDAL, PROJ, SQLite and the rest) are prebuilt and stay
uninstrumented.

## Building

Use a separate build directory for each sanitizer. To skip a second vcpkg install, reuse the
packages of an existing build (`build/` here):

```bash
export VCPKG_ROOT=$PWD/vcpkg
cmake -S . -B build-asan \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_INSTALLED_DIR=$PWD/build/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF \
  -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS_RELEASE=-g -O1" "-DCMAKE_C_FLAGS_RELEASE=-g -O1" \
  -DDDB_ENABLE_ASAN=ON -DDDB_ENABLE_UBSAN=ON
cmake --build build-asan --target ddb_unit_tests ddbcmd -j$(nproc)
```

Add `ddbtest` to the targets to run the integration tests too; the library objects are compiled
once for both test binaries.

For TSan, use `-B build-tsan -DDDB_ENABLE_TSAN=ON` instead of the two ASan/UBSan options.
`-O1` keeps the instrumented test suite reasonably fast; the module adds
`-fno-omit-frame-pointer` for readable stacks. The external tools (Obj2Tiles, build-lod,
untwine) are not built by CMake: copy them next to `ddbtest` if the integration tests that use
them should run. The unit tests need none of them.

## Running

The suppression files are in `tests/`. They list third-party code only; a report in `src/`,
`apps/` or `tests/` is a bug to fix, never an entry to add.

ASan + UBSan, unit and CLI tests (what the `sanitizers_asan_ubsan` CI job runs). Timing
assertions mean nothing under instrumentation, so `DDB_SKIP_TIMING_TESTS=1` skips them. ctest runs
the tests from the build root, so the suppression paths must be absolute:

```bash
cd build-asan
export DDB_SKIP_TIMING_TESTS=1 \
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  LSAN_OPTIONS=suppressions=$PWD/../tests/lsan.supp \
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:suppressions=$PWD/../tests/ubsan.supp
ctest -L unit --output-on-failure && ctest -R '^cli\.' --output-on-failure
```

The integration tests, with the same environment: `./ddbtest --gtest_shuffle`.

ctest runs each test in its own process, so a leak report names the test that caused it. Run as
one process (`./ddb_unit_tests`, `./ddbtest`), LeakSanitizer checks at process exit: a leak fails
the whole run (exit code 23) and does not point to the test that caused it; bisect with
`--gtest_filter`. A test that `fork()`s a child
which leaves through `std::exit()` must call `disableLeakCheckAtExit()`
(`tests/sanitizer_hooks.h`) in the child: the child holds a copy of the parent's heap without
the parent's other threads, so its leak check reports the parent's live objects.

TSan, concurrency tests (what the nightly `TSan (nightly)` workflow runs, report-only). Build
`ddb_unit_tests ddbtest` in `build-tsan`, then:

```bash
cd build-tsan
export TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1:suppressions=$PWD/../tests/tsan.supp
./ddb_unit_tests --gtest_filter='BuildLockTest.*:concurrentAdd.*:PooledConnections.*:OpenIndex.concurrent*:TilerCache.concurrent*:Shutdown.*:CancelToken.*:Cancellation.*:CAbiLog.*'
./ddbtest --gtest_filter='CopcNodeCache.concurrent*:CopcNodeReader.concurrent*:PointCloudTiler.concurrent*:PointRasterizer.concurrent*:ParallelLoop.*:ParallelIndex.*:IsBuildActiveTest.RealBuildInThread:tiles3dBuild.cleanupNeverStripsTilesFromConcurrentModelBuild:Defects.Target_RandomHelperGivesEachThreadItsOwnEngine:Defects.Target_SingletonsAgreeUnderConcurrentFirstUse:Defects.Target_DsmCache*'
```

TSan only sees code it instrumented: frames of the prebuilt dependencies are missing from its
stacks, and their atomics and internal locks are invisible to it. When a report shows only
DroneDB frames around a library call, catch it in gdb (`break __tsan_on_report`) to get the
full stack before deciding whether it is ours.

On kernels with a high `vm.mmap_rnd_bits`, older TSan runtimes abort at start with
"unexpected memory mapping"; run the binary under `setarch -R` in that case. CI lowers it instead
(`sudo sysctl -w vm.mmap_rnd_bits=28`) before running the GCC 13 sanitizer runtimes.

## Coverage

```bash
cmake -S . -B build-cov ... -DCMAKE_BUILD_TYPE=Debug -DDDB_ENABLE_COVERAGE=ON
cmake --build build-cov --target ddb_unit_tests ddbtest -j$(nproc)
(cd build-cov && ./ddb_unit_tests && ./ddbtest)
gcovr -r . build-cov --filter src/ --html-details -o coverage.html   # or lcov
```
