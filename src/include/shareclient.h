/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SHARECLIENT_H
#define SHARECLIENT_H

#include <string>
#include <vector>
#include <cpr/cpr.h>
#include "registry.h"
#include "ddb_export.h"
#include "mio.h"

namespace ddb
{

    class ShareClient
    {

        std::string token;
        ddb::Registry *registry;
        std::string resultUrl;

    public:
        DDB_DLL ShareClient(ddb::Registry *registry);

        DDB_DLL void Init(const std::string &tag, const std::string &password, const std::string &datasetName = "", const std::string &datasetDescription = "");
        DDB_DLL void Upload(const std::string &path, const fs::path &filePath, const utils::UploadCallback &cb = nullptr);
        DDB_DLL std::string Commit();

        DDB_DLL std::string getToken() const;
        DDB_DLL std::string getResultUrl() const;
    };

}
#endif // SHARECLIENT_H
