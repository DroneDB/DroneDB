/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SHARESERVICE_H
#define SHARESERVICE_H

#include <functional>
#include <string>
#include <vector>

#include "ddb_export.h"
#include "registry_auth.h"
#include "registry_client.h"

namespace ddb
{

    struct ShareFileProgress
    {
        std::string filename = "";
        size_t txBytes = 0;
        size_t totalBytes = 0;
    };
    typedef std::function<bool(const std::vector<ShareFileProgress *> &files,
                               size_t txBytes, size_t totalBytes)>
        ShareCallback;

    class ShareService
    {
        RegistryDeps deps;

    public:
        DDB_DLL explicit ShareService(const RegistryDeps &registryDeps = {});

        // Uploads the files (directories are skipped) to a new share of the Registry in `tag`
        // and returns the URL of the result. The login goes through withAuthenticatedRegistry:
        // `credentials` is asked when none are saved or the saved ones are refused.
        DDB_DLL std::string share(const std::vector<std::string> &input,
                                  const std::string &tag,
                                  const std::string &password,
                                  CredentialProvider &credentials,
                                  const std::string &cwd = "",
                                  const ShareCallback &cb = nullptr,
                                  bool sslVerify = true);
    };

} // namespace ddb
#endif // SHARESERVICE_H
