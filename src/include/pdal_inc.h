/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PDAL_INC_H
#define PDAL_INC_H

// Include this before any PDAL header.
//
// On Windows, PDAL's headers mark their API __declspec(dllexport) for every user, not only
// while PDAL itself is built. ddb.dll would then export each PDAL inline function, template
// instantiation and static guard variable it compiles (564 symbols), and only the C API of
// ddb.h may be exported (scripts/check-abi.py exports). An empty macro links the same way:
// dllexport on a declaration without a definition is a plain extern declaration, which
// resolves through PDAL's import library. On other platforms the PDAL macros stay as they
// are: the linker version script limits the exports to the manifest.

#ifdef PDAL_EXPORT
#error "pdal_inc.h must be included before any PDAL header"
#endif

#include <pdal/util/pdal_util_export.hpp>

#ifdef _WIN32
#undef PDAL_EXPORT
#undef PDAL_DLL
#undef PDAL_EXPORT_DEPRECATED
#define PDAL_EXPORT
#define PDAL_DLL
#define PDAL_EXPORT_DEPRECATED [[deprecated]]
#endif

#endif
