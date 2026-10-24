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

    // What to share
    struct ShareRequest
    {
        std::vector<std::string> input{};  // the files (directories are skipped), already expanded
        std::string tag{};                 // the Registry, organization and dataset: <host>/<org>/<ds>
        std::string password{};            // of the share ("" = none)
        std::string cwd{};                 // the folder the paths of input are relative to
        ShareCallback callback{};          // progress, optional
        bool sslVerify = true;
    };

    class ShareService
    {
        RegistryDeps deps;

    public:
        DDB_DLL explicit ShareService(const RegistryDeps &registryDeps = {});

        // Uploads the files (directories are skipped) to a new share of the Registry in `tag`
        // and returns the URL of the result. The login goes through withAuthenticatedRegistry:
        // `credentials` is asked when none are saved or the saved ones are refused.
        DDB_DLL std::string share(const ShareRequest &request, CredentialProvider &credentials);
    };

} // namespace ddb
#endif // SHARESERVICE_H
