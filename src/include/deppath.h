/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DEPPATH_H
#define DEPPATH_H

// Internal (not exported) helpers shared by the builders that resolve file
// dependencies written inside a source file: OBJ mtllib, MTL textures, and any
// converter that writes such references. Every reference goes through the same
// contract:
//   - normalise: '\' -> '/', then lexically_normal ("./a.png" -> "a.png");
//   - an absolute reference (POSIX root, drive letter, UNC, file: URI) can never
//     be satisfied by uploading a file into the dataset;
//   - a reference that, resolved against its base folder, leaves the dataset
//     root can never be satisfied either. "../" that stays inside the dataset is
//     allowed. The root check only runs when the dataset root is known: files
//     produced by a converter in a temp folder are checked for absolute
//     references only.
// Unsatisfiable references fail the build (plain AppException) instead of
// leaving it pending; see buildInternal() in build_dispatch.cpp.

#include <cctype>
#include <string>
#include <system_error>

#include "fs.h"

namespace ddb {
namespace deppath {

// Returns the normalised generic ('/'-separated) form of a dependency reference.
// An empty or "." result means the reference does not name a file.
inline std::string normalize(const std::string& ref) {
    std::string s = ref;
    for (auto& c : s)
        if (c == '\\')
            c = '/';

    std::string out = fs::path(s).lexically_normal().generic_string();

    // lexically_normal keeps a trailing separator ("dir/" -> "dir/"); a file
    // reference never needs it.
    while (out.size() > 1 && out.back() == '/')
        out.pop_back();
    return out;
}

// True when the (normalised or raw) reference is absolute on any platform.
// A model made on Windows can carry "C:\tex.png" and must be recognised as
// absolute on Linux too.
inline bool isAbsolute(const std::string& ref) {
    if (ref.empty())
        return false;
    if (ref[0] == '/' || ref[0] == '\\')
        return true;  // POSIX root, UNC prefix, or a root-relative Windows path
    if (ref.size() >= 2 && std::isalpha(static_cast<unsigned char>(ref[0])) && ref[1] == ':')
        return true;  // drive letter ("C:\x", "C:/x", "C:x")
    if (ref.size() >= 5) {
        std::string scheme = ref.substr(0, 5);
        for (auto& c : scheme)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (scheme == "file:")
            return true;
    }
    const fs::path p(ref);
    return p.is_absolute() || p.has_root_name() || p.has_root_directory();
}

// Lexical absolute form used for containment checks (no symlink resolution, so
// it works for files that do not exist yet). A trailing separator is dropped so
// "/data/ds/" and "/data/ds" compare equal.
inline fs::path absoluteNormal(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec)
        abs = p;
    abs = abs.lexically_normal();
    if (abs.has_relative_path() && abs.filename().empty())
        abs = abs.parent_path();
    return abs;
}

// True when `p` lies inside (or is) `root`, lexically.
inline bool isInside(const fs::path& p, const fs::path& root) {
    const fs::path rel = absoluteNormal(p).lexically_relative(absoluteNormal(root));
    if (rel.empty())
        return false;  // different root names (e.g. another drive)
    return *rel.begin() != "..";
}

// Converts a missing-dependency name reported relative to the folder of the
// source file (e.g. "m.mtl", "mats/tex.png") into the dataset-root-relative
// form stored in ".pending" (e.g. "sub/m.mtl", "sub/mats/tex.png").
// `sourceEntryPath` is the dataset-relative path of the source file.
inline std::string toDatasetRelative(const std::string& sourceEntryPath, const std::string& name) {
    const fs::path parent = fs::path(normalize(sourceEntryPath)).parent_path();
    const std::string n = normalize(name);
    if (parent.empty())
        return n;
    return normalize((parent / fs::path(n)).generic_string());
}

}  // namespace deppath
}  // namespace ddb

#endif  // DEPPATH_H
