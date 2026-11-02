/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TAGMANAGER_H
#define TAGMANAGER_H

#include "dbops.h"
#include "entry.h"

namespace ddb
{

    // The file of the dataset's tag, in its .ddb folder
    inline constexpr const char *kTagsFile = "tags.json";

    class TagManager
    {
        Database *db;

    public:
        TagManager(Database *db) : db(db) {}

        /// The tag of the dataset, "" when it has none; never creates the tags file
        std::string getTag() const;
        void setTag(const std::string &tag);
    };

} // namespace ddb

#endif // TAGMANAGER_H
