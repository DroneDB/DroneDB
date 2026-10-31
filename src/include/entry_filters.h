/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef ENTRY_FILTERS_H
#define ENTRY_FILTERS_H

// The filters a caller applies to the entries of an index: by type, by extension and by name
// (DDBList, DDBRescan, ddb list / rescan). The parsers turn the comma-separated text of the C
// API and the CLI into a list; the predicates are what dbops runs on every entry.

#include <string>
#include <vector>

#include "entry_types.h"

namespace ddb::filters {

// Parse a comma-separated list of entry type names into a vector of EntryType
// Returns an empty vector if types is null or empty
// Throws InvalidArgsException if an unknown type is found, or if Directory is specified and
// allowDirectory is false (rescans skip directories)
std::vector<EntryType> parseEntryTypeList(const char* types, bool allowDirectory = false);

// True if name matches pattern, where '*' matches any characters (also none) and '?' one
// (UTF-8) character. Letters match regardless of case: Unicode simple case folding
// (utf8::foldCase) when both are valid UTF-8, else a byte comparison with ASCII letters in any
// case.
bool matchesNamePattern(const std::string& name, const std::string& pattern);

// Parse a comma-separated list of file extensions ("dwg,.DXF,copc.laz") into
// lowercase extensions without the leading dot. Empty items are skipped.
// Returns an empty vector if extensions is null or empty
// Throws InvalidArgsException if an item is a bare dot or contains a path separator,
// a wildcard or whitespace
std::vector<std::string> parseExtensionList(const char* extensions);

// True if path ends with "." + one of extensions (lowercase, without the dot, as
// returned by parseExtensionList). Comparison ignores case
bool hasAnyExtension(const std::string& path, const std::vector<std::string>& extensions);

}  // namespace ddb::filters

#endif  // ENTRY_FILTERS_H
