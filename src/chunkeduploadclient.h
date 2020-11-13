/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CHUNKEDUPLOADCLIENT_H
#define CHUNKEDUPLOADCLIENT_H

#include <string>
#include <vector>
#include "net.h"
#include "registry.h"
#include "shareclient.h"
#include "ddb_export.h"
#include "mio.h"

namespace ddb{

class ChunkedUploadClient{

    int sessionId;
    ddb::Registry *registry;
    ddb::ShareClient *shareClient;
    ddb::Registry* registry_;
    ddb::ShareClient* share_client_;

public:
    
    DDB_DLL ChunkedUploadClient(ddb::Registry* registry, ddb::ShareClient *shareClient);

    DDB_DLL int StartSession(int chunks, size_t size);
    DDB_DLL void UploadToSession(int index, std::istream input);
    DDB_DLL void CloseSession(const fs::path& filePath, std::string& path);

    DDB_DLL int getSessionId() const;

};

}
#endif // CHUNKEDUPLOADCLIENT_H
