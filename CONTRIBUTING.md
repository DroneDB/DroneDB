# Contributing

:tada: First off, thanks for considering being a contributor! :tada:

## Comment Style Conventions

DroneDB uses a consistent commenting style across the C++ codebase. Please follow these conventions:

### License Header

Every file in `src/`, `apps/`, `cmd/`, and `tests/` must start with the MPL 2.0 license header:

```cpp
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
```

Files in `vendor/` are excluded (they retain their own licenses).

### API Documentation (Headers)

Use **Doxygen `/** ... */`** (Javadoc-style) blocks for public API functions, classes, structs, and enums in header files:

```cpp
/**
 * @brief Short one-line description
 *
 * Longer description if needed.
 *
 * @param name Description of the parameter
 * @return Description of the return value
 * @throws ExceptionType When and why it throws
 */
```

For single-line descriptions, a compact form is acceptable:

```cpp
/** Get library version */
```

### Inline Comments (Source Files)

Use `//` single-line comments for inline explanations within `.cpp` files:

```cpp
// Forward declaration for stale lock detection helper
// Returns true if the given file exists and has non-zero size.
```

Short end-of-line notes may use C-style `/* ... */`:

```cpp
y = fabs(y); /* The result doesn't depend on the sign of y */
```

### Section Dividers

Use `// ───` for section separators in headers and source files:

```cpp
// ─── Internal types ─────────────────────────────────────────────────────────
// ─── Public API ─────────────────────────────────────────────────────────────
```

Avoid decorative characters like `═══` or `§` in section dividers.

### What NOT to Use

- **`///`** (triple-slash): Do not use. Convert to `/** ... */` instead.
- **`//!`** (exclamation): Do not use.
- **MSDN XML style** (`<summary>`, `<param name="">`): Not recognized by Doxygen. Use `@brief`, `@param` instead.
- **Multi-line `/* ... */`** for API docs: Use `/** ... */` with Doxygen tags instead.

## How can I contribute?

DDB contributors are expected to follow the [Collective Code of Construction Contract (C4)](https://rfc.zeromq.org/spec:42/C4/). You should read the document before making a pull request.

Take a look at the list of [open issues](https://github.com/uav4geo/ddb/issues) to find out how you can help.

## Code of Conduct

This project adheres to the [Contributor Covenant](CONDUCT.md). By participating, you are expected to uphold this code. Please report unacceptable behavior to the project maintainers.

## Overlay ports (vcpkg-overlays)

`vcpkg-overlays/` holds vcpkg ports that replace the ones of the vcpkg submodule
(`vcpkg-configuration.json` lists the folder in `overlay-ports`). An overlay is a copy of the
port with the patches DroneDB needs; every patch carries a comment in its `portfile.cmake` that
says why it exists and when it can go. When the vcpkg submodule is bumped, compare each overlay
with the new upstream port and carry the changes over.

- **`vcpkg-overlays/pdal`**: PDAL 2.10.1 with `gdal-error-handler.patch`. Stock PDAL installs
  its own GDAL error handler in `Stage::setupLog` and restores the saved one in the first
  `Stage::stopLogging`; the `stopLogging` of the outer stage of a nested pair (the text reader
  preview runs a statistics filter inside it) then restores a null handler, GDAL's default, and
  an exception thrown between `startLogging` and `stopLogging` leaves PDAL's handler installed.
  After either, the handler of libddb no longer receives GDAL errors, so the log callback of a
  host misses them. The patch installs the handler once for the outermost running stage, restores
  the saved handler when the last one stops, never installs a null handler, and keeps the start
  and stop of the logging paired when an exception leaves a stage. The tests are in
  `tests/unit/c_abi_log_test.cpp` (`CAbiLogPdal`).
  The same change is pending as a pull request to the PDAL project (not opened yet; the link goes
  here when it exists). Remove the overlay, and with it `vcpkg-overlays/pdal`, when the vcpkg
  port of a PDAL release that contains the fix is the one the submodule builds, and the
  `CAbiLogPdal` tests pass with the stock port.

## Helper programs

libddb starts a few programs as child processes: `metadata-util` (EXIF and XMP, Exiv2), `bm3d`
(multiresolution models) and `gdal-util` (reads the files of users with GDAL, behind the
confinement of `vendor/sandbox`). Each lives in its own repository and is a git submodule in
`vendor/` (`metadata-util`, `bm3d`) or `apps/` (`gdal-util`); the submodule is built inside this
tree and nowhere else (`gdal-util` embeds libddb with `ddb_embed_library`). A program found by
`DDB_METADATA_UTIL_PATH`, `DDB_BM3D_PATH` or `DDB_GDAL_UTIL_PATH`, next to the executable or on
`PATH` is accepted when its protocol major version matches.

To add a helper:

1. Put its sources in `vendor/<name>` or `apps/<name>` with a `CMakeLists.txt` whose target is
   named `<name>`, and add the subdirectory to `vendor/CMakeLists.txt` or `apps/CMakeLists.txt`.
2. List it in `scripts/helper-tools.txt` as `<name> | <version source>`: `package` when it must
   report the version of the package (`<name> --version` prints `<name> <version> ...`), or the
   path of a file whose first line is its version. The CMake build (a name that is no target stops
   the configure), the Debian and Windows packaging scripts, the package check
   (`scripts/check-helper-tools.py`), the long path check and the clean-container smoke test of CI
   all read that list, so nothing else names the program.
3. List its licence text, and those of the code built into it, in `scripts/helper-licences.txt`
   under a name that contains `.LICENSE`. A GPL helper must keep its code out of libddb and
   `ddbcmd`: `scripts/check-gpl-linkage.py` (and the `.ps1` of it for Windows) fails on a helper
   of the list that none of its rules covers, so say there which rule holds it.
4. If it speaks a protocol, put `PROTOCOL.md` in its repository and a schema of the response in
   `tests/contract/schemas`.

## Bumping a helper submodule

Move the submodule to the new commit (`git -C <path> checkout <tag>`, then commit the pointer in
this repository), and in the same change:

- **vendor/metadata-util**, **vendor/bm3d**: check the version the program reports against its
  source in `scripts/helper-tools.txt`; for bm3d `scripts/check-bm3d-pin.sh` also checks the
  release tag the source offer of `THIRD_PARTY_LICENSES.md` names.
- **apps/gdal-util**: its `PROTOCOL.md` and `src/include/raster_protocol.h` describe the same
  protocol and change together. A change of the protocol that older hosts or helpers cannot read
  raises the major version in both, and `tests/contract/schemas/gdal-util-probe.schema.json` and
  its fixture follow. The operations it runs are in libddb (`src/library/raster_ops.cpp`), so
  a bump of the helper that changes what an operation reads or writes is a libddb change as well.
- Run the unit tests and `scripts/check-helper-tools.py` against the new build.

A release is not done before this is checked on the image that runs in production, because the
confinement of `gdal-util` depends on the kernel and on the container settings of the host (Landlock,
seccomp), and a layer that the host lacks is skipped in the default `best-effort` mode and
reported only in the log:

```
docker exec <container> gdal-util --sandbox-report
```

Every layer the report lists must be active. `DDB_GDAL_SANDBOX=strict` makes `gdal-util` refuse to
start when a layer is missing (use it where the host is known to support all of them);
`best-effort` is the default; `off` applies nothing and is for tests and debugging only.

## Opening files with GDAL

The host (the Registry, `ddbcmd`, the tests) never opens a file of a user with GDAL: it asks
`gdal-util` (`src/include/raster_helper.h`) and reads only what libddb wrote, through
`openArtefactRaster`, `openArtefactVector` or `reopenWrittenRaster` (`src/include/raster_artefacts.h`).
The operations of the helper open user files with `openUserRaster` and `openUserVector`
(`src/include/gdal_open_policy.h`), which throw in any process that did not call
`enableUserFileOpens()`. `scripts/check-gdal-opens.py` (run by CI before the build; `--self-test`
checks the check) enforces both rules on the sources:

- `GDALOpen*`, `GDALIdentifyDriver*`, `OGROpen*` and the string `/vsicurl` may appear in `src/` and
  `apps/` (`apps/gdal-util` aside) only in the files of `ALLOWED_FILES`. Use the functions above;
  a file that really needs a raw open is added to `ALLOWED_FILES` with a one-line reason that says
  why none of them fits, and the review reads it. An entry that no longer matches anything fails
  the check, so remove it with the open.
- `enableUserFileOpens()` is called only by `gdal-util` at its start and by the test support that
  runs a test body in a forked child (`tests/user_open_child.cpp`, the `USER_OPEN_TEST` macros).
  A test that needs to open a user file uses those macros or the real `gdal-util`; nothing else
  calls it.

## Bumping vendor/spz and vendor/spark

- **vendor/spz** (nianticlabs): bump by checking out a tagged commit, or a reviewed commit of
  upstream `main` when the fixes it needs are not in a tag yet. CI runs the gsplat tests
  (including the SPZ v2/v3/v4 fixture tests). The `.spz` export stays gzip-based SPZ v3,
  which every SPZ reader reads (`kDeliverySpzVersion` in `splat_convert.cpp`). After the
  bump, check the CI gsplat test count shows **0 skipped**.
- **vendor/spark** (build-lod producer): policy is to pin to a release tag matching Hub's
  `@sparkjsdev/spark` npm version, bumping both in the same release cycle. CI warns on
  non-tag pins. build-lod reads every SPZ version (gzip v1-3 and NGSP/ZSTD v4) since
  v2.4.0, so a v4 upload is built as it is: a spark pin older than v2.4.0 fails
  `gsplat.buildAcceptsAV4Spz`.
- If the **"Verify Spark RAD format pin"** CI step fails: the spark RAD writer
  (`rust/spark-lib/src/rad.rs`) changed since the last review. Review encoding handling in
  `src/library/rad.cpp` before merging, then refresh the pin file with the command printed
  by the failing step.
- Local verification: run `scripts/build-buildlod.*`, then
  `build/ddbtest --gtest_filter=gsplat.*` with `DDB_REQUIRE_BUILDLOD=1` and confirm
  0 skipped; spot-check a gsplat dataset in Hub.

