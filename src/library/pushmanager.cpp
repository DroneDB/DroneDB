/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "pushmanager.h"

#include "exceptions.h"
#include "json.h"
#include "mio.h"

namespace ddb
{

    std::string PushManager::endpoint(const std::string &name) const
    {
        return "/orgs/" + this->organization + "/ds/" + this->dataset + "/push/" + name;
    }

    PushInitResponse PushManager::init(const std::string &registryStampChecksum, const json &dbStamp)
    {
        const auto res = registry->postForm(endpoint("init"),
                                            {{"checksum", registryStampChecksum}, {"stamp", dbStamp.dump()}},
                                            AuthScheme::Bearer);
        registry->expectOk(res);

        json j = json::parse(res.body);

        if (j.contains("pullRequired") && j["pullRequired"].get<bool>())
            throw PullRequiredException("The remote has new changes. Use \"ddb pull\" to get the latest changes before pushing.");
        if (!j.contains("neededFiles") || !j.contains("token") || !j.contains("neededMeta"))
            registry->fail(res);

        PushInitResponse pir;
        pir.neededFiles = j["neededFiles"].get<std::vector<std::string>>();
        pir.neededMeta = j["neededMeta"].get<std::vector<std::string>>();
        pir.token = j["token"].get<std::string>();
        return pir;
    }

    void PushManager::upload(const std::string &fullPath, const std::string &file, const std::string &token)
    {
        const auto res = registry->postMultipart(endpoint("upload"),
                                                 {HttpPart::fromFile("file", io::fromUtf8(fullPath)),
                                                  HttpPart::text("path", file),
                                                  HttpPart::text("token", token)},
                                                 AuthScheme::Bearer);
        registry->expectOk(res);
    }

    void PushManager::meta(const json &metaDump, const std::string &token)
    {
        const auto res = registry->postForm(endpoint("meta"),
                                            {{"meta", metaDump.dump()}, {"token", token}},
                                            AuthScheme::Bearer);
        registry->expectOk(res);
    }

    void PushManager::commit(const std::string &token)
    {
        const auto res = registry->postForm(endpoint("commit"), {{"token", token}}, AuthScheme::Bearer);
        registry->expectOk(res);
    }

} // namespace ddb
