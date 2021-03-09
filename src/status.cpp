/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dbops.h"
#include "status.h"


#include <ddb.h>
#include <mio.h>

#include "exceptions.h"

namespace ddb
{

	void statusIndex(Database* db, const FileStatusCallback& cb)
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
					cb(Modified, relPath.generic());
				}
			}
			else
			{
				cb(Deleted, relPath.generic());				
			}
		}

        try{
            for (auto i = fs::recursive_directory_iterator(directory);
                i != fs::recursive_directory_iterator();
                ++i) {

                auto path = i->path();
                auto p = path.generic_string();

                // Skips already checked folders
                if (checkedPaths.count(p) == 1)
                    continue;

                if (p == directory.generic_string())
                {
                    LOGD << "Skipping parent folder";
                    continue;
                }

                // Skip .ddb
                if (path.filename() == DDB_FOLDER)
                    i.disable_recursion_pending();

                if (p.find(DDB_FOLDER) != std::string::npos)
                {
                    LOGD << "Skipping ddb folder";
                    continue;
                }

                cb(NotIndexed, io::Path(p).relativeTo(directory).generic());

            }
        }catch(const fs::filesystem_error &e){
            throw FSException(e.what());
        }
		
	}

} // namespace ddb
