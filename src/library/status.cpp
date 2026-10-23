/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dataset_walk.h"
#include "dbops.h"
#include "status.h"

#include "ddb.h"
#include "entry_path.h"
#include "mio.h"

#include "exceptions.h"
#include "hash.h"
#include "logger.h"

#include <cmath>

namespace ddb
{

    FileStatus checkUpdate(Entry &e, const fs::path &p, double dbMtime,
                           const std::string &dbHash, double *fileMtime)
    {

        if (!exists(p))
            return Deleted;

        const bool folder = fs::is_directory(p);

        if (folder)
            return NotModified;

        // Did it change? Sub-second times catch an edit made in the same second as the
        // previous add (same size, same whole-second time)
        const double mtime = io::Path(p).getModifiedTimePrecise();
        e.mtime = static_cast<time_t>(std::floor(mtime));
        if (fileMtime != nullptr)
            *fileMtime = mtime;

        if (!io::sameModifiedTime(dbMtime, mtime))
        {
            LOGD << io::toUtf8(p) << " modified time ( " << std::fixed << dbMtime
                 << " ) differs from file value: " << mtime;

            e.hash = Hash::fileSHA256(io::toUtf8(p));

            if (dbHash != e.hash)
            {
                LOGD << io::toUtf8(p) << " hash differs (old: " << dbHash
                     << " | new: " << e.hash << ")";
                return Modified;
            }
        }

        return NotModified;
    }

    void statusIndex(Database *db, const FileStatusCallback &cb)
    {

        const fs::path directory = db->rootDirectory();

        auto q = db->query("SELECT path,mtime,hash FROM entries");

        std::set<std::string> checkedPaths;

        while (q->fetch())
        {
            io::Path relPath = io::fromUtf8(q->getText(0));
            auto p = directory / relPath.get(); // TODO: does this work on Windows?
            Entry e;

            auto path = io::toGenericUtf8(p);

            checkedPaths.insert(path);

            const auto status = checkUpdate(e, p, q->getDouble(1), q->getText(2));

            cb(status, relPath.generic());
        }

        // What `ddb add` would find: the same walk, so a file that add skips is never reported
        walkDataset(directory, {}, [&](const fs::directory_entry &entry)
        {
            const fs::path &path = entry.path();

            // Skips already checked folders
            if (checkedPaths.count(io::toGenericUtf8(path)) == 1 || !hasIndexableName(path))
                return;

            cb(NotIndexed, io::Path(path).relativeTo(directory).generic());
        });
    }

} // namespace ddb
