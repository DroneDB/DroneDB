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

    this->registry->ensureTokenValidity();

    net::Response res =
        net::POST(this->registry->getUrl("/org/" + this->organization + "/ds/" +
                                         this->dataset + "/push/init"))
            .multiPartFormData({"file", ddbPathArchive.string()})
            .authToken(this->registry->getAuthToken())
            .send();

    if (res.status() != 200) this->registry->handleError(res);
}

DDB_DLL void PushManager::upload(const std::string& file) { 


    this->registry->ensureTokenValidity();

    net::Response res =
        net::POST(this->registry->getUrl("/org/" + this->organization + "/ds/" + this->dataset + "/push/upload"))
            .multiPartFormData({"file", file})
            .authToken(this->registry->getAuthToken())
            .send();

    if (res.status() != 200) this->registry->handleError(res);
        
}

DDB_DLL void PushManager::commit()
{
    this->registry->ensureTokenValidity();

    net::Response res =
        net::POST(this->registry->getUrl("/org/" + this->organization + "/ds/" +
                                         this->dataset + "/push/commit"))
            .authToken(this->registry->getAuthToken())
            .send();

    if (res.status() != 200) this->registry->handleError(res);
}

}  // namespace ddb
