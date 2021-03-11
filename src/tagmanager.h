/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TAGMANAGER_H
#define TAGMANAGER_H

#include "dbops.h"
#include "ddb_export.h"
#include "entry.h"

namespace ddb {

#define TAGSFILE "tags.json"

class TagManager {
    fs::path ddbFolder;

   public:
    TagManager(const fs::path& ddbFolder) : ddbFolder(ddbFolder) {
        //
    }

    DDB_DLL std::string getTag();
    DDB_DLL void setTag(const std::string& tag);
};

}  // namespace ddb

#endif  // TAGMANAGER_H
