/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "pushmanager.h"

#include "dbops.h"
#include "fs.h"
#include "mio.h"
#include "registry.h"
#include "registryutils.h"
#include "userprofile.h"
#include "utils.h"

namespace ddb {
    

DDB_DLL std::vector<std::string> PushManager::init(
    const fs::path& ddbPathArchive) {

    throw NotImplementedException("Not implemented");

}

DDB_DLL void PushManager::upload(const std::string& file) { 
    throw NotImplementedException("Not implemented");
}

DDB_DLL void PushManager::commit()
{
    throw NotImplementedException("Not implemented");
}

}  // namespace ddb
