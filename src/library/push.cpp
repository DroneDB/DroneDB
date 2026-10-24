/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "dbops.h"
#include "exceptions.h"
#include "mio.h"
#include "pushmanager.h"
#include "registry_ops.h"
#include "syncmanager.h"
#include "tagmanager.h"

namespace ddb {

void pushDataset(RegistryClient& client, const std::string& path, std::ostream& out) {
    auto db = open(path, true);

    TagManager tagManager(db.get());
    SyncManager syncManager(db.get());

    // Get our tag using tagmanager
    const auto tag = tagManager.getTag();

    if (tag.empty())
        throw IndexException("Cannot push if no tag is specified");

    const auto tagInfo = RegistryUtils::parseTag(tag);

    try {
        // 3) Get dataset info
        const auto dsInfo = client.getDatasetInfo(tagInfo.organization, tagInfo.dataset);

        out << "Pushing to '" << tag << "'" << std::endl;
    } catch (RegistryNotFoundException& ex) {

        out << "Pushing to new '" << tag << "'" << std::endl;
    }

    // Initialize server push

    // Call POST endpoint passing database stamp
    PushManager pushManager(&client, tagInfo.organization, tagInfo.dataset);

    // The stamp of the Registry as of the last sync: the empty dataset on a first push, which a
    // Registry that has no such dataset does not look at and one with content answers with
    // "pull first"
    const std::string registryStampChecksum = syncManager.getLastStamp(tagInfo.registryUrl)["checksum"];

    const auto pir = pushManager.init(registryStampChecksum, db->getStamp());

    // Push meta
    if (pir.neededMeta.size() > 0) {
        out << "Transferring metadata (" << pir.neededMeta.size() << ")" << std::endl;
        pushManager.meta(db->getMetaManager()->dump(pir.neededMeta), pir.token);
    }

    const auto basePath = db->rootDirectory();

    for (const auto& file : pir.neededFiles) {
        const auto fullPath = basePath / io::fromUtf8(file);

        out << "Transfering '" << file << "'" << std::endl;

        // Foreach of the needed files call POST endpoint
        pushManager.upload(io::toGenericUtf8(fullPath), file, pir.token);
    }

    // When done call commit endpoint
    pushManager.commit(pir.token);

    // Update stamp
    syncManager.setLastStamp(tagInfo.registryUrl, db.get());

    out << "Push completed" << std::endl;
}

void push(const std::string& registry, bool sslVerify, CredentialProvider& credentials, const RegistryDeps& deps) {
    const auto currentPath = io::toUtf8(fs::current_path());

    const auto db = open(currentPath, true);
    const auto registryUrl = datasetRegistryUrl(db.get(), registry, "push");

    withAuthenticatedRegistry(registryUrl, sslVerify, credentials, [&](RegistryClient& client) {
        pushDataset(client, currentPath, std::cout);
    }, {}, deps);
}

}  // namespace ddb
