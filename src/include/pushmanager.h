/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PUSHMANAGER_H
#define PUSHMANAGER_H

#include <string>
#include <vector>

#include "json_fwd.h"
#include "registry_client.h"

namespace ddb
{

    struct PushInitResponse
    {
        std::vector<std::string> neededFiles;
        std::vector<std::string> neededMeta;
        std::string token;
    };

    // The four requests of a push (init, meta, upload, commit). None of them is tried again by
    // the client: a push is a sequence the server tracks by token, and an upload that may have
    // arrived must not be sent twice behind the caller's back. A push that fails is started again.
    class PushManager
    {
        RegistryClient *registry;
        std::string organization;
        std::string dataset;

        std::string endpoint(const std::string &name) const;

    public:
        PushManager(RegistryClient *client,
                    const std::string &org,
                    const std::string &ds)
            : registry(client), organization(org), dataset(ds)
        {
        }

        PushInitResponse init(const std::string &registryStampChecksum, const json &dbStamp);
        void upload(const std::string &fullPath, const std::string &file, const std::string &token);
        void meta(const json &metaDump, const std::string &token);
        void commit(const std::string &token);

        std::string getOrganization() { return this->organization; }
        std::string getDataset() { return this->dataset; }
    };

} // namespace ddb
#endif // PUSHMANAGER_H
