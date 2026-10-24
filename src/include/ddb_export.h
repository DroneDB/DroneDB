/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_EXPORT_H
#define DDB_EXPORT_H

/* DDB_DLL marks the functions of the C API (ddb.h): the only symbols libddb exports. The library
 * is built with hidden visibility, so a symbol without DDB_DLL stays inside it.
 *
 * Which one it is depends on who includes the header:
 *   DDB_STATIC_DEFINE     the code is linked with the library objects instead of libddb (the CLI
 *                         and the test binaries): nothing to mark.
 *   DDB_BUILDING_LIBRARY  the library itself is being built: export (Windows: dllexport).
 *   neither               a user of the shared library: Windows dllimport, elsewhere the
 *                         visibility attribute.
 *
 * One file for every platform, because it is published with ddb.h and ddb_version.h (release
 * assets): a header made by CMake's generate_export_header() holds the macros of the platform
 * it was generated on, and the Linux one does not compile with MSVC. */
#if defined(DDB_STATIC_DEFINE)
#define DDB_DLL
#elif defined(_WIN32) || defined(__CYGWIN__)
#if defined(DDB_BUILDING_LIBRARY)
#define DDB_DLL __declspec(dllexport)
#else
#define DDB_DLL __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define DDB_DLL __attribute__((visibility("default")))
#else
#define DDB_DLL
#endif

#endif /* DDB_EXPORT_H */
