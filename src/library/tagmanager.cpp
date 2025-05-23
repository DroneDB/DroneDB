/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "tagmanager.h"

#include "ddb.h"
#include "mio.h"

#include "dbops.h"
#include "exceptions.h"
#include "registryutils.h"

namespace ddb
{

    std::string TagManager::getTag()
    {
        const auto path = db->ddbDirectory() / TAGSFILE;

        LOGD << "Path = " << path;

        if (!exists(path))
        {
            LOGD << "Path does not exist, creating empty file";

            std::ofstream out(path, std::ios_base::out);
            out << "{}";
            out.close();
            return "";
        }

        std::ifstream i(path);
        json j;
        i >> j;

        if (!j.contains("tag"))
            return "";

        TagComponents t = RegistryUtils::parseTag(j["tag"]);
        return t.tag();
    }
    void TagManager::setTag(const std::string &tag)
    {
        const auto path = db->ddbDirectory() / TAGSFILE;

        const auto tg = RegistryUtils::parseTag(tag);

        LOGD << "Path = " << path;
        LOGD << "Setting tag '" << tg.fullTag();

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

        j["tag"] = tg.fullTag();

        std::ofstream out(path, std::ios_base::out | std::ios_base::trunc);
        out << j.dump(4);
        out.close();
    }
} // namespace ddb
