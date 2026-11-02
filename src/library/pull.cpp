/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <boolinq/boolinq.h>

#include "dbops.h"
#include "delta.h"
#include "exceptions.h"
#include "mio.h"
#include "registry_ops.h"
#include "syncmanager.h"
#include "tagmanager.h"

namespace ddb {

namespace {

// Downloads into `folder` the files the delta adds that the dataset does not have yet: not the
// directories, and not the files whose content is in the index or in the files that go away
void downloadMissingFiles(RegistryClient& client, const TagComponents& tagInfo, const Delta& delta,
                          Database* db, const fs::path& folder, std::ostream& out) {
    auto localMap = computeDeltaLocals(delta, db, io::toUtf8(folder));

    const auto filesToDownload =
        boolinq::from(delta.adds)
            .where([&localMap](const AddAction& add) {
                return !add.isDirectory() && localMap.find(add.hash) == localMap.end();
            })
            .select([](const AddAction& add) { return add.path; })
            .toStdVector();

    // Download all the missing files
    client.downloadFiles(tagInfo.organization, tagInfo.dataset, filesToDownload, io::toUtf8(folder), out);
}

void reportConflicts(std::vector<Conflict>& conflicts, std::ostream& out) {
    out << "Found conflicts, but don't worry! Make a copy of the conflicting entries and use "
           "--keep-theirs or --keep-ours to finish the pull:"
        << std::endl
        << std::endl;

    for (auto& c : conflicts) {
        out << "C\t" << c.path << " (" << c.description() << ")" << std::endl;
    }
}

}  // namespace

void pullDataset(RegistryClient& client, const std::string& path, const MergeStrategy mergeStrategy, std::ostream& out) {
    auto db = open(path, true);
    TagManager tagManager(db.get());

    // Get our tag using tagmanager
    const auto tag = tagManager.getTag();

    if (tag.empty())
        throw IndexException("Cannot pull if no tag is specified");

    const auto tagInfo = RegistryUtils::parseTag(tag);

    out << "Pulling from '" << tag << "'" << std::endl;

    const auto tempDdbFolder =
        db->ddbDirectory() / "tmp" / "pull_cache" / io::fromUtf8(tagInfo.organization + "-" + tagInfo.dataset);

    if (fs::exists(tempDdbFolder)) {
        // TODO: there might be ways to resume downloads if a user CTRL+Cs
        io::assureIsRemoved(tempDdbFolder);
    }

    // Get stamp from registry
    json remoteStamp = client.getStamp(tagInfo.organization, tagInfo.dataset);

    // Perform local diff using delta method using last stamp
    SyncManager sm(db.get());
    const auto delta = getDelta(remoteStamp, sm.getLastStamp(tagInfo.registryUrl));

    if (!delta.empty()) {
        out << "Delta: files (+" << delta.adds.size() << ",-" << delta.removes.size()
            << "), meta (+" << delta.metaAdds.size() << ",-" << delta.metaRemoves.size() << ")"
            << std::endl;
    }

    json remoteMetaDump = json::array();

    if (delta.metaAdds.size() > 0) {
        // Get meta dump
        remoteMetaDump = client.getMetaDump(tagInfo.organization, tagInfo.dataset, delta.metaAdds);
    }

    const auto tempNewFolder = tempDdbFolder / std::to_string(time(nullptr));

    // Let's download only if we have anything to download
    if (!delta.adds.empty())
        downloadMissingFiles(client, tagInfo, delta, db.get(), tempNewFolder, out);

    // TODO: speedup: we should be able to check for conflicts
    // before we download the files

    // Apply changes to local files
    DeltaApplication application;
    application.sourcePath = tempNewFolder;
    application.mergeStrategy = mergeStrategy;
    application.sourceMetaDump = remoteMetaDump;
    auto conflicts = applyDelta(delta, db.get(), application, out);
    io::assureIsRemoved(tempNewFolder);

    if (conflicts.size() == 0) {
        // No errors? Update stamp
        sm.setLastStamp(tagInfo.registryUrl, remoteStamp);
    } else {
        reportConflicts(conflicts, out);
    }

    db->close();

    // Cleanup
    io::assureIsRemoved(tempDdbFolder);
    io::assureIsRemoved(tempNewFolder);

    // Inform user if nothing was needed
    if (delta.empty()) {
        out << "Everything up-to-date" << std::endl;
    } else {
        out << "Pull completed" << std::endl;
    }
}

void pull(const std::string& registry, MergeStrategy mergeStrategy, bool sslVerify,
          CredentialProvider& credentials, std::ostream& out, const RegistryDeps& deps) {
    const auto currentPath = io::toUtf8(fs::current_path());

    const auto db = open(currentPath, true);
    const auto registryUrl = datasetRegistryUrl(db.get(), registry, "pull");

    withAuthenticatedRegistry(registryUrl, sslVerify, credentials, [&](RegistryClient& client) {
        pullDataset(client, currentPath, mergeStrategy, out);
    }, {}, deps);
}

}  // namespace ddb
