/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef STL_H
#define STL_H

#include "entry_types.h"
#include "fs.h"

namespace ddb {

/**
 * @brief Classify an .stl file as a triangle mesh.
 *
 * An STL has no magic number and a binary file may well start with "solid" in its
 * 80-byte header, so the size decides first:
 *  - binary: the size is exactly 84 + 50 * N bytes, N being the little-endian
 *    triangle count at offset 80, and N is not zero;
 *  - ASCII: the file starts with `solid` (an optional UTF-8 byte order mark and
 *    leading white space are skipped), the rest of that line is the solid's name, and
 *    the next token is `facet` followed by `normal`.
 *
 * Best-effort and never throws, like @ref identifyPly: a missing, unreadable, empty,
 * truncated or otherwise unrecognised file is EntryType::Generic, so it can still be
 * indexed but is not offered for a build that could only fail.
 *
 * @param stlFile Path to the .stl file.
 * @return EntryType::Model for a binary or ASCII STL with at least one triangle,
 *         EntryType::Generic otherwise.
 */
EntryType identifyStl(const fs::path& stlFile);

}  // namespace ddb

#endif  // STL_H
