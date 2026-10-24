/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DATASET_WALK_H
#define DATASET_WALK_H

// The one rule of what is in a dataset folder tree, shared by everything that lists it: adding
// (getIndexPathList, expandGlobPatterns), describing (getPathList, ddb info) and comparing with
// the index (statusIndex). Before, three copies of the loop each decided for themselves, and
// `ddb status` listed files that `ddb add` skips.

#include <functional>

#include "fs.h"

namespace ddb
{

    struct WalkOptions
    {
        // Levels below the start folder to list: 0 = the whole tree, 1 = its children only
        int levels = 0;
    };

    /**
     * Calls visit for every file and folder below `folder` (never for the folder itself), in
     * the directory iterator's order. Not listed, and not entered:
     *  - ".ddb" folders (isReservedSegment);
     *  - hidden and system files and folders (Windows attributes; isHiddenOrSystem);
     *  - folders the process cannot read (skipped, not an error).
     * A filesystem error aborts the walk with an FSException. The entries are not filtered by
     * hasIndexableName: that is the caller's decision, because indexing keeps looking below a
     * folder it does not index.
     */
    void walkDataset(const fs::path &folder, const WalkOptions &options,
                     const std::function<void(const fs::directory_entry &entry)> &visit);

    /// True for a Windows hidden or system file or folder (Thumbs.db, desktop.ini, $RECYCLE.BIN);
    /// always false elsewhere, where "hidden" is only a naming convention that datasets use
    bool isHiddenOrSystem(const fs::path &path);

    /// False for a name that indexing never stores: it contains a path separator ('\' on POSIX,
    /// where it is a legal file name character; entry paths could not name it, isEntryPathSeparator)
    bool hasIndexableName(const fs::path &path);

} // namespace ddb

#endif // DATASET_WALK_H
