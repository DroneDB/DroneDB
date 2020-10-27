/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dbops.h"
#include "status.h"

#include <mio.h>

#include "exceptions.h"

namespace ddb
{

    void statusIndex(Database *db)
    {

        const fs::path directory = rootDirectory(db);

        auto q = db->query("SELECT path,mtime,hash FROM entries");

        std::set<std::string> checkedPaths;

        while (q->fetch())
        {
            io::Path relPath = fs::path(q->getText(0));
            auto p = directory / relPath.get(); // TODO: does this work on Windows?
            Entry e;

            auto path = p.generic_string();

            checkedPaths.insert(path);

            if (exists(p))
            {
                if (checkUpdate(e, p, q->getInt64(1), q->getText(2)))
                {
                    std::cout << "M\t" << relPath.string() << std::endl;
                }
            }
            else
            {
                std::cout << "-\t" << relPath.string() << std::endl;
            }
        }

        auto tmp = std::vector<std::string>();
        tmp.push_back(directory.string());

        auto paths = getIndexPathList(directory, tmp, true, true);

        for (auto const &path : paths)
        {

            auto p = path.generic_string();

            // Skips already checked folders
            if (checkedPaths.count(p) == 1)
                continue;

            if (p == directory.generic_string())
            {
                LOGD << "Skipping parent folder";
                continue;
            }

            if (p.find(".ddb") != std::string::npos)
            {
                LOGD << "Skipping ddb folder";
                continue;
            }

            std::cout << "+\t" << io::Path(p).relativeTo(directory).string() << std::endl;
        }
    }

} // namespace ddb
