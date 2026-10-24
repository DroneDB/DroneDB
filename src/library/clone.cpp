/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "dbops.h"
#include "exceptions.h"
#include "mio.h"
#include "registry_ops.h"
#include "tagmanager.h"

namespace ddb {

void cloneDataset(RegistryClient& client, const std::string& organization, const std::string& dataset,
                  const std::string& folder, std::ostream& out) {
    if (fs::exists(io::fromUtf8(folder)) && !fs::is_empty(io::fromUtf8(folder))) {
        throw FSException(folder + " already exists");
    }

    io::assureFolderExists(io::fromUtf8(folder));

    client.ensureTokenValidity();

    initIndex(folder);
    const auto db = ddb::open(folder, false);

    TagManager tagManager(db.get());
    tagManager.setTag(client.getUrl("/" + organization + "/" + dataset));
    pullDataset(client, folder, MergeStrategy::KeepTheirs, out);
}

void clone(const TagComponents& tag, const std::string& folder, bool sslVerify, CredentialProvider& credentials,
           const RegistryDeps& deps) {
    if (fs::exists(io::fromUtf8(folder)) && !fs::is_empty(io::fromUtf8(folder))) {
        std::cout << "Cannot clone in folder '" + folder + "' because it already exists" << std::endl;
        return;
    }

    std::cout << "Cloning dataset '" << tag.organization << "/" << tag.dataset << "' from registry '"
              << tag.registryUrl << "' to folder '" << folder << "'" << std::endl;

    // A public dataset needs no login: the credentials are asked for when the server says 401
    AuthenticationOptions options;
    options.tryAnonymousFirst = true;

    withAuthenticatedRegistry(
        tag.registryUrl, sslVerify, credentials,
        [&](RegistryClient& client) {
            try {
                cloneDataset(client, tag.organization, tag.dataset, folder, std::cout);
            } catch (const AuthException&) {
                // What the refused attempt started is removed: the next one needs an empty folder
                io::assureIsRemoved(io::fromUtf8(folder));
                throw;
            }
        },
        options, deps);
}

}  // namespace ddb
