/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PUSHMANAGER_H
#define PUSHMANAGER_H

#include <string>
#include <vector>

#include "ddb_export.h"
#include "mio.h"
#include "net.h"
#include "registry.h"
#include "shareclient.h"

namespace ddb {

class PushManager {
    ddb::Registry* registry;
    std::string organization;
    std::string dataset;

   public:
    DDB_DLL PushManager(ddb::Registry* registry,
                        const std::string& organization,
                        const std::string& dataset) {
        this->registry = registry;
        this->organization = organization;
        this->dataset = dataset;
    }

    DDB_DLL std::vector<std::string> init(const fs::path& ddbPathArchive);
    DDB_DLL void upload(const std::string& file);
    DDB_DLL void commit();

    DDB_DLL std::string getOrganization() { return this->organization; }
    DDB_DLL std::string getDataset() { return this->dataset; }
};

}  // namespace ddb
#endif  // PUSHMANAGER_H
