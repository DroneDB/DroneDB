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

    int sessionId = 0;
    int chunks = 0;
    size_t size = 0;

    std::string fileName;
    ddb::Registry *registry;
    ddb::ShareClient *shareClient;

public:
    
    DDB_DLL ChunkedUploadClient(ddb::Registry* registry, ddb::ShareClient *shareClient);

    DDB_DLL int StartSession(int chunks, size_t size, const std::string& fileName);
    DDB_DLL void UploadToSession(int index, std::istream* input,
                                 size_t byteOffset, size_t byteLength,
                                 const UploadCallback& cb);
    DDB_DLL void CloseSession(const std::string& path, const fs::path& filePath);

    DDB_DLL int getSessionId() const;
    
};

}
#endif // CHUNKEDUPLOADCLIENT_H
