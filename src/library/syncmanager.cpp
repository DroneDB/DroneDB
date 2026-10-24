/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "syncmanager.h"
#include <fstream>

#include "json.h"

#include "ddb.h"
#include "mio.h"

#include "dbops.h"
#include "exceptions.h"
#include "registryutils.h"

namespace ddb
{

    json SyncManager::getLastStamp(const std::string &registry)
    {
        const auto path = this->db->ddbDirectory() / SYNCFILE;

        LOGD << "Path = " << path;
        LOGD << "Registry = " << registry;

        if (registry.length() == 0)
            throw InvalidArgsException("Registry cannot be null");

        // Reading leaves nothing behind: a dataset that was never synced has no file
        if (exists(path))
        {
            std::ifstream i(path);
            json j;
            i >> j;

            if (j.contains(registry))
                return j[registry];
        }

        // Never synced with this Registry: nothing is known to be in common. The baseline of the
        // first pull must not be the dataset itself (a file the Registry does not have would be
        // a file it deleted, and the pull would delete it here), nor the checksum of the first
        // push (the Registry would be told it has the dataset it is about to receive).
        return Database::emptyStamp();
    }

    void SyncManager::setLastStamp(const std::string &registry, Database *sourceDb)
    {
        if (sourceDb != nullptr)
        {
            setLastStamp(registry, sourceDb->getStamp());
        }
        else
        {
            setLastStamp(registry, db->getStamp());
        }
    }

    void SyncManager::setLastStamp(const std::string &registry, const json &stamp)
    {
        const auto path = this->db->ddbDirectory() / SYNCFILE;

        LOGD << "Path = " << path;
        LOGD << "Registry = " << registry;

        if (registry.length() == 0)
            throw InvalidArgsException("Registry cannot be null");

        if (!exists(path))
        {
            std::ofstream out(path, std::ios_base::out);
            out << "{}";
            out.close();
        }

        std::ifstream i(path);
        json j;
        i >> j;
        i.close();

        j[registry] = stamp;

        std::ofstream out(path, std::ios_base::out | std::ios_base::trunc);
        out << j.dump(4);
        out.close();
    }

} // namespace ddb
