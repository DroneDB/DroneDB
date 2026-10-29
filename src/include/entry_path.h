/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ENTRY_PATH_H
#define ENTRY_PATH_H

#include <string>
#include <string_view>
#include <vector>

#include "fs.h"
#include "json_fwd.h"

// The dataset path rules: what a dataset-relative entry path may be, and its one
// normalized spelling. libddb owns them; Registry calls DDBNormalizePath (ddb.h) so both sides
// accept and reject the same paths. The golden vectors in tests/contract/vectors/entry-paths.json
// pin every rule below, and both test suites run them.
//
// Rules, checked in this order (the first one that applies names the rejection):
//   1. "empty"     the path is "" (no byte at all)
//   2. "nul"       it contains a NUL byte: a native API would see a shorter path
//   3. "encoding"  it is not valid UTF-8 (utf8.h): JSON (every libddb output) cannot carry it
//   4. "absolute"  it starts with '/' or '\' (a POSIX root, a Windows root, a UNC "\\server\share"
//                  or a device path "\\?\"), or with a drive "X:" (Windows, "C:\x" and "C:x")
//   5. "traversal" a segment is ".."
//   6. "empty"     nothing is left once the empty and "." segments are dropped (".", "./", "//"
//                  is "absolute" by rule 4, "a/.." is "traversal" by rule 5)
//   7. "reserved"  a segment is reserved: ".ddb" at any depth (isReservedSegment), or the first
//                  segment is one of the caller's ReservedRules::firstSegment (Registry passes
//                  ".uploads", which exists at the dataset root only)
// An accepted path is returned with '/' between the segments, no leading, trailing or repeated
// separator and no "." segment.
//
// Separators: '/' and '\' both separate segments, as in Registry. libddb never indexes a file
// whose name contains '\' (planAddCandidates skips it), so a normalized path, which cannot
// contain one, never names a file that indexing would skip.
//
// The ".ddb" segment is matched as the file systems a dataset may live on open it, on every
// platform alike (one rule, so a path is accepted or refused everywhere): ASCII letters in any
// case, trailing dots and spaces dropped, and what follows a ':' (an NTFS stream name) ignored,
// so ".DDB", ".Ddb", ".ddb.", ".ddb " and ".ddb::$INDEX_ALLOCATION" are reserved.
//
// Not rules (deliberately): other names are compared byte for byte (".ddbx", ".ddb_notes.txt"
// and "photos/.uploads/x" are ordinary, and so are the caller's names); no Unicode
// normalization (NFC and NFD spellings are different paths); whitespace is part of a name
// (Registry's whitespace-only check is its own).
namespace ddb {

/// Extra reserved names, from the caller
struct ReservedRules {
    // Names reserved as the FIRST segment only (e.g. ".uploads"); compared byte for byte
    std::vector<std::string> firstSegment;

    /// The rules of a JSON array of names, e.g. [".uploads"]. Each name is a single non-empty
    /// segment: no separator, NUL, "." or "..". An empty or null text means no extra rule.
    /// Throws InvalidArgsException otherwise.
    static ReservedRules fromJson(const char* text);
};

/// The outcome of normalizeEntryPath: an accepted path, or a rejection code
struct EntryPathResult {
    std::string path;           // the normalized path ("" when rejected)
    std::string_view rejected;  // a stable code (see above), empty when accepted

    bool accepted() const { return rejected.empty(); }

};

/// {"path": "a/b"} or {"rejected": "<code>"}: the output of DDBNormalizePath
void to_json(json& j, const EntryPathResult& result);

/// True for a separator of a dataset-relative path: '/' or '\'
bool isEntryPathSeparator(char c) noexcept;

/// True when a path segment (one name, no separator) is reserved at any depth: ".ddb"
/// (DDB_FOLDER) as a file system may open it (see above). libddb never indexes anything at or
/// below such a segment.
bool isReservedSegment(std::string_view segment) noexcept;
/// The same rule for one component of a filesystem path (fs::path::filename(), or an element of
/// the iteration over a path)
bool isReservedSegment(const fs::path& segment);

/// True when a '/'-separated path has a "." or ".." segment (a literal path argument, before any
/// normalization: the caller refuses it)
bool hasDotNotation(const std::string& path);

/// Normalizes a dataset-relative path, or rejects it (rules above)
EntryPathResult normalizeEntryPath(std::string_view path, const ReservedRules& rules = {});

/// The normalized spelling of a dataset-relative entry path, or an InvalidPathException ("Invalid
/// <what> '<path>': <rejection code>"; the path is left out of the message when it is not valid
/// UTF-8). The one place the C API and the library refuse an entry path: `what` names the
/// argument in the message ("path", "source path"). Not for filesystem paths (files being added,
/// output files, dataset roots), which are not entry paths.
std::string requireEntryPath(std::string_view path, std::string_view what = "path", const ReservedRules& rules = {});

/// An optional entry path: "" is none and stays "" (the dataset itself, or no path), anything
/// else is requireEntryPath
std::string optionalEntryPath(std::string_view path, std::string_view what = "path");

/// A dataset-relative folder: "" and a path that names the root only (".", "./") are the dataset
/// root, "", anything else is requireEntryPath
std::string requireEntryFolder(std::string_view folder, std::string_view what = "folder");

}  // namespace ddb

#endif  // ENTRY_PATH_H
