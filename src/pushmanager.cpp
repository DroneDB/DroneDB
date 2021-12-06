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

DDB_DLL PushInitResponse PushManager::init(const std::string &registryStampChecksum, const json &dbStamp) {
    this->registry->ensureTokenValidity();

    net::Response res =
        net::POST(this->registry->getUrl("/orgs/" + this->organization + "/ds/" +
                                         this->dataset + "/push/init"))
            .formData({"checksum", registryStampChecksum,
                       "stamp", dbStamp.dump()})
            .authToken(this->registry->getAuthToken())
            .send();

    if (res.status() != 200) this->registry->handleError(res);

    json j = res.getJSON();

    if (j.contains("pullRequired") && j["pullRequired"].get<bool>()) throw PullRequiredException("The remote has new changes. Use \"ddb pull\" to get the latest changes before pushing.");
    if (!j.contains("neededFiles") || !j.contains("token")) this->registry->handleError(res);

    PushInitResponse pir;
    pir.filesList = j["neededFiles"].get<std::vector<std::string>>();
    pir.token = j["token"].get<std::string>();
    return pir;
}

DDB_DLL void PushManager::upload(const std::string& fullPath, const std::string& file, const std::string &token) {
    this->registry->ensureTokenValidity();

    net::Response res =
        net::POST(this->registry->getUrl("/orgs/" + this->organization + "/ds/" +
                                         this->dataset + "/push/upload"))
            .multiPartFormData({"file", fullPath}, {"path", file, "token", token})
            .authToken(this->registry->getAuthToken())
            .send();

    if (res.status() != 200) this->registry->handleError(res);
}

DDB_DLL void PushManager::commit(const std::string &token) {
    this->registry->ensureTokenValidity();

    net::Response res =
        net::POST(this->registry->getUrl("/orgs/" + this->organization + "/ds/" +
                                         this->dataset + "/push/commit"))
            .formData({"token", token})
            .authToken(this->registry->getAuthToken())
            .send();

    if (res.status() != 200) this->registry->handleError(res);
}

}  // namespace ddb
