/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef STATUS_H
#define STATUS_H

#include "entry.h"

#include "dbops.h"

namespace ddb
{
    enum FileStatus
    {
        NotIndexed,
        Deleted,
        Modified,
        NotModified
    };

    /**
     * Compares a file with its index row (modification time, then hash when the time differs).
     * Sets e.mtime, and e.hash when it hashed the file. fileMtime, when given, receives the
     * file's modification time with sub-second precision (to store in the index).
     */
    FileStatus checkUpdate(Entry &e, const fs::path &p, double dbMtime, const std::string &dbHash,
                           double *fileMtime = nullptr);

    typedef std::function<void(const FileStatus status, const std::string &file)> FileStatusCallback;

    void statusIndex(Database *db, const FileStatusCallback &cb);

}

#endif // STATUS_H
