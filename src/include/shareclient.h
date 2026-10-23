/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SHARECLIENT_H
#define SHARECLIENT_H

#include <functional>
#include <string>

#include "ddb_export.h"
#include "fs.h"
#include "registry_client.h"

namespace ddb
{

    // Called while a file is uploaded with its name and the bytes sent and to send; false aborts
    typedef std::function<bool(std::string &fileName, size_t txBytes, size_t totalBytes)> UploadCallback;

    // The three requests of a share (init, upload, commit). None is tried again by the client: an
    // upload is a body that may have arrived, and the share is a sequence the server tracks by token.
    class ShareClient
    {

        std::string token;
        RegistryClient *registry;
        std::string resultUrl;

    public:
        DDB_DLL ShareClient(RegistryClient *client);

        DDB_DLL void Init(const std::string &tag, const std::string &password, const std::string &datasetName = "", const std::string &datasetDescription = "");
        DDB_DLL void Upload(const std::string &path, const fs::path &filePath, const UploadCallback &cb = nullptr);
        DDB_DLL std::string Commit();

        DDB_DLL std::string getToken() const;
        DDB_DLL std::string getResultUrl() const;
    };

}
#endif // SHARECLIENT_H
