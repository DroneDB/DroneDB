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
#include <cpr/cpr.h>

namespace ddb
{

    DDB_DLL PushInitResponse PushManager::init(const std::string &registryStampChecksum, const json &dbStamp)
    {
        this->registry->ensureTokenValidity();

        auto res = cpr::Post(cpr::Url(this->registry->getUrl("/orgs/" + this->organization + "/ds/" +
                                                             this->dataset + "/push/init")),
                             cpr::Payload{{"checksum", registryStampChecksum},
                                          {"stamp", dbStamp.dump()}},
                             utils::authHeader(this->registry->getAuthToken()),
                             cpr::VerifySsl(this->registry->getSslVerify()));

        if (res.status_code != 200)
            this->registry->handleError(res);

        json j = json::parse(res.text);

        if (j.contains("pullRequired") && j["pullRequired"].get<bool>())
            throw PullRequiredException("The remote has new changes. Use \"ddb pull\" to get the latest changes before pushing.");
        if (!j.contains("neededFiles") || !j.contains("token") || !j.contains("neededMeta"))
            this->registry->handleError(res);

        PushInitResponse pir;
        pir.neededFiles = j["neededFiles"].get<std::vector<std::string>>();
        pir.neededMeta = j["neededMeta"].get<std::vector<std::string>>();
        pir.token = j["token"].get<std::string>();
        return pir;
    }

    DDB_DLL void PushManager::upload(const std::string &fullPath, const std::string &file, const std::string &token)
    {
        this->registry->ensureTokenValidity();

        auto res = cpr::Post(cpr::Url(this->registry->getUrl("/orgs/" + this->organization + "/ds/" +
                                                             this->dataset + "/push/upload")),
                             cpr::Multipart{{"file", cpr::File{fullPath}},
                                            {"path", file},
                                            {"token", token}},
                             utils::authHeader(this->registry->getAuthToken()),
                             cpr::VerifySsl(this->registry->getSslVerify()));

        if (res.status_code != 200)
            this->registry->handleError(res);
    }

    void PushManager::meta(const json &metaDump, const std::string &token)
    {
        auto res = cpr::Post(cpr::Url(this->registry->getUrl("/orgs/" + this->organization + "/ds/" +
                                                             this->dataset + "/push/meta")),
                             cpr::Payload{{"meta", metaDump.dump()},
                                          {"token", token}},
                             utils::authHeader(this->registry->getAuthToken()),
                             cpr::VerifySsl(this->registry->getSslVerify()));

        if (res.status_code != 200)
            this->registry->handleError(res);
    }

    DDB_DLL void PushManager::commit(const std::string &token)
    {
        this->registry->ensureTokenValidity();

        auto res = cpr::Post(cpr::Url(this->registry->getUrl("/orgs/" + this->organization + "/ds/" +
                                                             this->dataset + "/push/commit")),
                             cpr::Payload{{"token", token}},
                             utils::authHeader(this->registry->getAuthToken()),
                             cpr::VerifySsl(this->registry->getSslVerify()));

        if (res.status_code != 200)
            this->registry->handleError(res);
    }

} // namespace ddb
