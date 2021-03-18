/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <authcredentials.h>
#include <delta.h>
#include <mio.h>
#include <registryutils.h>
#include <syncmanager.h>
#include <tagmanager.h>
#include <userprofile.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "dbops.h"
#include "exceptions.h"
#include "../vendor/boolinq/boolinq.h"

namespace ddb {


void applyDelta(const Delta& delta, const std::string& folder, const std::string& newFilesFolder)
{
    throw NotImplementedException("Not implemented yet");
}


void pull(const std::string& registry, const bool force) {

    /*

    -- Pull Workflow --

    1) Get our tag using tagmanager
    2) Get our last sync time for that specific registry using syncmanager
    3) Get dataset mtime
        3.1) Authenticate if not
        3.2) Call endpoint
    4) Alert if dataset_mtime < last_sync (it means we have more recent changes
    than server, so the pull is pointless or potentially dangerous)
    5) Get ddb from registry
        5.1) Call endpoint
        5.2) Unzip archive in temp folder
    6) Perform local diff using delta method
    7) Download all the missing files
        7.1) Call download endpoint with file list (to test)
        7.2) Unzip archive in temp folder
    8) Apply changes to local files
    9) Replace ddb database
    10) Update last sync time

    */

    LOGD << "Pull from " << registry;
        
    // 2) Get our last sync time for that specific registry using syncmanager    

    const auto currentPath = std::filesystem::current_path();
    auto db = open(currentPath.string(), true);
    const auto ddbPath = fs::path(db->getOpenFile()).parent_path();

    LOGD << "Ddb folder = " << ddbPath;

    TagManager tagManager(ddbPath);
    SyncManager syncManager(ddbPath);

    // 1) Get our tag using tagmanager
    const auto tag = tagManager.getTag();

    // 2) Get our last sync time for that specific registry using syncmanager    
    const auto lastSync = syncManager.getLastSync(registry);

    LOGD << "Tag = " << tag;
    LOGD << "LastSync = " << lastSync;

    const auto tagInfo = RegistryUtils::parseTag(tag);

    Registry r(tagInfo.registryUrl);

    // 3) Get dataset mtime
    const auto mtime =
        r.getLastModifiedTime(tagInfo.organization, tagInfo.dataset);

    LOGD << "Dataset mtime = " << mtime;
    
    // 4) Alert if dataset_mtime < last_sync (it means we have more recent changes
    //    than server, so the pull is pointless or potentially dangerous)
    if (mtime < lastSync && !force)
        throw AppException(
            "Cannot pull if dataset changes are older than ours. Use force "
            "parameter to override (CAUTION)");

    const auto tempDdbFolder = fs::temp_directory_path() / "ddb_temp_folder" /
                               (tagInfo.organization + "-" + tagInfo.dataset);

    LOGD << "Temp ddb folder = " << tempDdbFolder;

    // 5) Get ddb from registry
    r.downloadDdb(tagInfo.organization, tagInfo.dataset, tempDdbFolder.string());

    LOGD << "Remote ddb downloaded";

    auto source = open(tempDdbFolder.string(), true);

    // 6) Perform local diff using delta method
    const auto delta = getDelta(source.get(), db.get());

    LOGD << "Delta:";

    json j = delta;
    LOGD << j.dump();

    const auto tempNewFolder = fs::temp_directory_path() / "ddb_new_folder" /
                               (tagInfo.organization + "-" + tagInfo.dataset);

    LOGD << "Temp new folder = " << tempNewFolder;
    
    const auto filesToDownload =
        boolinq::from(delta.adds)
            .where([](const AddAction& add) { return add.type != Directory; })
            .select([](const AddAction& add) { return add.path; })
            .toStdVector();

    LOGD << "Files to download:";
    j = filesToDownload;
    LOGD << j.dump();

    // 7) Download all the missing files
    r.downloadFiles(tagInfo.organization, tagInfo.dataset, filesToDownload,
                    tempNewFolder.string());

    LOGD << "Files downloaded, applying delta";

    // 8) Apply changes to local files
    applyDelta(delta, ddbPath.parent_path().string(), tempNewFolder.string());

    LOGD << "Replacing ddb folder";

    // 9) Replace ddb database
    copy(tempDdbFolder, ddbPath);

    LOGD << "Updating syncmanager and tagmanager";

    // 10) Update last sync time
    syncManager.setLastSync(registry);
    tagManager.setTag(tag);

    LOGD << "Pull done";

}

}  // namespace ddb
