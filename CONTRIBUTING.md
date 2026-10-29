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

