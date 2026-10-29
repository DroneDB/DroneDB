/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MZIP_H
#define MZIP_H

#include <cstdint>
#include <functional>
#include <string>

namespace ddb::zip
{

    // What an extraction accepts. An archive that exceeds a limit is refused with a ZipException
    // before a single byte is written, because the limits are checked on the declared sizes of
    // the central directory, and a member that turns out larger than it declared is cut off.
    struct ExtractOptions
    {
        // More entries than this are refused (a directory tree of millions of files exhausts
        // inodes long before the bytes run out).
        uint64_t maxEntries = 1000000;

        // The sum of the declared uncompressed sizes may be at most this many times the size
        // of the archive file; 0 turns the check off. Deflate stops at about 1000:1, so a bomb
        // that fits a megabyte writes a gigabyte, while real tile archives stay near 1:1.
        uint64_t maxExpansionRatio = 20;

        // Called with the declared uncompressed total once the limits above have passed and
        // before anything is written, so a caller can refuse an archive that does not fit the
        // disk (it throws). The module itself knows nothing about disks or builds.
        std::function<void(uint64_t declaredTotal)> beforeWrite;

        // Refuses, on every platform, the entry names Windows cannot create (a segment ending in
        // a dot or a space, containing ':' or naming a device such as CON or COM1), so an archive
        // extracts to the same tree everywhere, which an archive libddb serves needs. Off, such
        // a name extracts where the file system takes it (a POSIX one takes them all).
        bool refuseWindowsUnsafeNames = true;

        // Decides from the name of an entry (as the archive spells it) whether the caller wants
        // it: an entry it returns false for throws a ZipException before anything is written,
        // like a name that leaves the folder. For the caller that knows which names its
        // archive may hold (a pull gets the files it asked for, never a ".ddb" folder).
        std::function<bool(const std::string &entryName)> acceptEntry;
    };

    // Extracts every entry of zipFile into outdir (created). An entry name that is absolute,
    // would leave outdir (Zip-Slip) or, unless options allow it, cannot be created on Windows
    // throws a ZipException before anything is written, as does an archive beyond the limits of
    // options. A member is
    // always written as a regular file, whatever its mode says (a symbolic link is its target text).
    void extractAll(const std::string &zipFile, const std::string &outdir,
                    const ExtractOptions &options = ExtractOptions());

}

#endif // MZIP_H
