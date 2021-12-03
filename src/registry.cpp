/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <boolinq/boolinq.h>

#include "registry.h"

#include "build.h"
#include "ddb.h"
#include "delta.h"
#include "mio.h"
#include "pushmanager.h"
#include "syncmanager.h"
#include "tagmanager.h"

#include "mzip.h"
#include "exceptions.h"
#include "json.h"
#include "net.h"
#include "url.h"
#include "userprofile.h"
#include "utils.h"

using homer6::Url;

namespace ddb {

Registry::Registry(const std::string &url) {
    std::string urlStr = url;

    if (urlStr.empty()) urlStr = std::string(DEFAULT_REGISTRY);

    Url u;

    // Always append https if no protocol is specified
    if (urlStr.find("https://") != 0 && urlStr.find("http://") != 0) {
        u.fromString("https://" + urlStr);
    } else {
        u.fromString(urlStr);
    }

    // Validate and set URL
    if (u.getScheme() != "https" && u.getScheme() != "http") {
        throw URLException("Registry URL can only be http/https");
    }

    const std::string port = u.getPort() != 80 && u.getPort() != 443
                                 ? ":" + std::to_string(u.getPort())
                                 : "";
    this->url = u.getScheme() + "://" + u.getHost() + port + u.getPath();

    LOGD << "Registry URL: " << this->url;
}

std::string Registry::getUrl(const std::string &path) const {
    return url + path;
}

std::string Registry::login() {
    const auto ac =
        UserProfile::get()->getAuthManager()->loadCredentials(this->url);

    if (ac.empty())
        throw InvalidArgsException("No stored credentials for registry at '" +
                                   this->url + "'");

    return login(ac.username, ac.password);
}

std::string Registry::login(const std::string &username,
                            const std::string &password) {
    net::Response res =
        net::POST(getUrl("/users/authenticate"))
            .formData({"username", username, "password", password})
            .send();

    json j = res.getJSON();

    if (res.status() == 200 && j.contains("token")) {
        const auto token = j["token"].get<std::string>();
        const auto expiration = j["expires"].get<time_t>();

        // Save for next time
        UserProfile::get()->getAuthManager()->saveCredentials(
            url, AuthCredentials(username, password));

        this->authToken = token;
        this->tokenExpiration = expiration;

        LOGD << "AuthToken = " << token;
        LOGD << "Expiration = " << expiration;

        return std::string(token);
    }
    if (j.contains("error")) {
        throw AuthException("Login failed: " + j["error"].get<std::string>());
    }

    throw AuthException("Login failed: host returned " +
                        std::to_string(res.status()));
}

void Registry::ensureTokenValidity() {
    if (this->authToken.empty())
        throw InvalidArgsException("No auth token is present");

    const auto now = std::time(nullptr);

    LOGD << "Now = " << now << ", expration = " << this->tokenExpiration
         << ", diff = " << now - this->tokenExpiration;

    // If the token is still valid we have nothing to do
    if (now < this->tokenExpiration) {
        LOGD << "Token still valid";
        return;
    }

    LOGD << "Token expired: re-login";
    // Otherwise login
    this->login();
}

bool Registry::logout() {
    return UserProfile::get()->getAuthManager()->deleteCredentials(url);
}

void Registry::clone(const std::string &organization,
                             const std::string &dataset,
                             const std::string &folder, std::ostream &out) {
    if (fs::exists(folder)){
        throw FSException(folder + " already exists");
    }

    io::assureFolderExists(folder);

    // Workflow
    // 2) Download zip in temp folder
    // 3) Create target folder
    // 3.1) If target folder already exists throw error
    // 4) Unzip in target folder
    // 5) Remove temp zip
    // 6) Update sync information

    this->ensureTokenValidity();

    const auto downloadUrl =
        this->url + "/orgs/" + organization + "/ds/" + dataset + "/download";

    LOGD << "Downloading dataset '" << dataset << "' of organization '"
         << organization << "'";
    LOGD << "To folder: " << folder;

    LOGD << "Download url = " << downloadUrl;

    const auto tempFile =
        io::Path(fs::path(folder) / utils::generateRandomString(8))
            .string() +
        ".tmp";

    LOGD << "Temp file = " << tempFile;

    auto start = std::chrono::system_clock::now();
    size_t prevBytes = 0;

    auto res = net::GET(downloadUrl)
                   .authCookie(this->authToken)
                   .progressCb([&start, &prevBytes, &out](size_t txBytes,
                                                          size_t totalBytes) {
                       if (txBytes == prevBytes) return true;

                       const auto now = std::chrono::system_clock::now();

                       const std::chrono::duration<double> dT = now - start;

                       if (dT.count() < 1) return true;

                       const auto dData = txBytes - prevBytes;
                       const auto speed = dData / dT.count();

                       out << "Downloading " << io::bytesToHuman(txBytes)
                           << " @ " << io::bytesToHuman(speed) << "/s\t\t\r";
                       out.flush();

                       prevBytes = txBytes;
                       start = now;

                       return true;
                   })
                   .downloadToFile(tempFile);

    if (res.status() != 200) this->handleError(res);

    out << std::endl;

    zip::extractAll(tempFile, folder, &out);

    io::assureIsRemoved(tempFile);

    const auto db = ddb::open(std::string(folder), false);
    syncLocalMTimes(db.get());

    SyncManager syncManager(db.get());
    TagManager tagManager(db.get());

    syncManager.setLastStamp(this->url);
    tagManager.setTag(this->url + "/" + organization + "/" + dataset);
}

std::string Registry::getAuthToken() { return std::string(this->authToken); }

time_t Registry::getTokenExpiration() { return this->tokenExpiration; }

Entry Registry::getDatasetInfo(const std::string &organization,
                                             const std::string &dataset) {
    this->ensureTokenValidity();

    const auto getUrl = url + "/orgs/" + organization + "/ds/" + dataset;

    LOGD << "Getting info of tag " << dataset << "/" << organization;

    auto res =
        net::GET(getUrl).authCookie(this->authToken).send();

    if (res.status() == 404)
        throw RegistryNotFoundException("Dataset not found");

    if (res.status() != 200) this->handleError(res);

    LOGD << "Data: " << res.getText();

    const auto j = res.getJSON();

    // TODO: Catch errors

    if (j.empty())
        throw RegistryException("Invalid empty response from registry");

    Entry e;
    e.fromJSON(j[0]);
    return e;
}

void Registry::downloadDdb(const std::string &organization,
                                   const std::string &dataset,
                                   const std::string &folder) {
    this->ensureTokenValidity();

    const auto downloadUrl =
        url + "/orgs/" + organization + "/ds/" + dataset + "/ddb";

    LOGD << "Download url = " << downloadUrl;

    char *buffer;
    size_t length;

    auto res = net::GET(downloadUrl)
                   .authCookie(this->authToken)
                   .downloadToBuffer(&buffer, &length);

    if (res.status() != 200) this->handleError(res);

    zip::extractAllFromBuffer(buffer, length, folder);
    free(buffer);

    LOGD << "Done";
}

void Registry::downloadFiles(const std::string &organization,
                                     const std::string &dataset,
                                     const std::vector<std::string> &files,
                                     const std::string &folder) {
    if (files.empty()) {
        LOGD << "Asked to download an empty list of files...";
        return;
    }

    this->ensureTokenValidity();

    auto downloadUrl =
        url + "/orgs/" + organization + "/ds/" + dataset + "/download";

    LOGD << "Download url = " << downloadUrl;

    if (files.size() == 1) {
        downloadUrl += "?path=" + files[0];

        const auto destPath = fs::path(folder) / files[0];

        io::createDirectories(destPath.parent_path());

        auto res = net::GET(downloadUrl)
                       .authCookie(this->authToken)
                       .downloadToFile(destPath.generic_string());

        if (res.status() != 200) this->handleError(res);

    } else {
        const auto tempFile = (fs::path(folder) / (utils::generateRandomString(8) + ".tmp"));
        io::assureFolderExists(tempFile.parent_path());

        // Joins path list
        const auto paths = utils::join(files);

        LOGD << "Paths = " << paths;

        auto res = net::POST(downloadUrl)
                       .authCookie(this->authToken)
                       .formData({"path", paths})
                       .downloadToFile(tempFile.string());

        if (res.status() != 200) this->handleError(res);

        LOGD << "Files archive downloaded, extracting";

        try {
            zip::extractAll(tempFile.string(), folder);
            io::assureIsRemoved(tempFile);

            LOGD << "Done";
        } catch (const std::runtime_error &e) {
            io::assureIsRemoved(tempFile);
            throw AppException(e.what());
        }
    }
}

void ensureParentFolderExists(const fs::path &folder) {
    if (folder.has_parent_path()) {
        const auto parentPath = folder.parent_path();
        io::assureFolderExists(parentPath);
    }
}

void applyDelta(const Delta &d, const fs::path &sourcePath, Database *destination, const MergeStrategy mergeStrategy, std::ostream& out) {
    std::vector<Conflict> conflicts;

    fs::path destPath = destination->rootDirectory();
    const std::string tmpFolderName = (fs::path(DDB_FOLDER) / "tmp" / utils::generateRandomString(8)).string();

    const auto tempPath = destPath / tmpFolderName;

    if (fs::exists(tempPath)){
        io::assureIsRemoved(tempPath);
    }
    io::assureFolderExists(tempPath);

    Entry e;

    json debug = d;
    LOGD << debug.dump(4);

    if (d.removes.empty()) {
        LOGD << "No removes in delta";
    } else {
        LOGD << "Working on removes";

        for (const auto &rem : d.removes) {
            LOGD << rem.toString();

            const auto dest = destPath / rem.path;

            LOGD << "Dest = " << dest;

            // Check if database has modified the entry to be deleted
            // if so, warn user and exit, unless a merge strategy
            // has been specified.

            // Currently we don't check if the file has been
            // modified on the FS, perhaps we should?
            bool indexed = true;

            if (getEntry(destination, rem.path, e)){
                if (rem.hash != e.hash){
                    if (mergeStrategy == MergeStrategy::DontMerge){
                        conflicts.push_back(Conflict(rem.path, ConflictType::RemoteDeleteLocalModified));
                        continue; // Skip
                    }else if (mergeStrategy == MergeStrategy::KeepOurs){
                        continue; // Skip
                    }else if (mergeStrategy == MergeStrategy::KeepTheirs){
                        // Continue as normal
                    }
                }
            }else{
                indexed = false;
            }

            if (fs::exists(dest)) {
                if (indexed) removeFromIndex(destination, { dest.string() });
                io::assureIsRemoved(dest);
                out << "D\t" << dest << std::endl;
            }
        }
    }

    if (d.adds.empty()) {
        LOGD << "No adds in delta";
    } else {
        LOGD << "Working on adds";

        for (const auto &add : d.adds) {
            LOGD << add.toString();

            const auto source = sourcePath / add.path;
            const auto dest = destPath / add.path;

            // Check if the database has a modified entry
            // for the same paths we are adding
            if (getEntry(destination, add.path, e)){
                if (add.hash != e.hash){
                    if (mergeStrategy == MergeStrategy::DontMerge){
                        conflicts.push_back(Conflict(add.path, ConflictType::BothModified));
                        continue; // Skip
                    }else if (mergeStrategy == MergeStrategy::KeepOurs){
                        continue; // Skip
                    }else if (mergeStrategy == MergeStrategy::KeepTheirs){
                        // Continue as normal
                    }
                }
            }

            if (add.isDirectory()) {
                io::createDirectories(dest);
            } else {
                io::copy(source, dest);
            }

            // TODO: this could be made faster for large files
            // by passing the already known hash instead
            // of computing it
            addToIndex(destination, { dest.string() },
                [&out](const Entry& e, bool updated) {
                    out << (updated ? "U" : "A") << "\t" << e.path << std::endl;
                    return true;
                });
        }
    }

    if (fs::exists(tempPath)) {
        io::assureIsRemoved(tempPath);
    }

    if (conflicts.size() > 0){
        throw MergeException(conflicts);
    }
}

void Registry::pull(const std::string &path, const MergeStrategy mergeStrategy,
                            std::ostream &out) {
    LOGD << "Pull from " << this->url;

    auto db = open(path, true);
    TagManager tagManager(db.get());

    // Get our tag using tagmanager
    const auto tag = tagManager.getTag();

    if (tag.empty()) throw IndexException("Cannot pull if no tag is specified");

    LOGD << "Tag = " << tag;

    const auto tagInfo = RegistryUtils::parseTag(tag);

    out << "Pulling from '" << tag << "'" << std::endl;

    const auto tempDdbFolder = db->ddbDirectory() / "tmp" / "pull_cache" /
        (tagInfo.organization + "-" + tagInfo.dataset);

    if (fs::exists(tempDdbFolder)){
        // TODO: there might be ways to resume downloads if a user CTRL+Cs
        io::assureIsRemoved(tempDdbFolder);
    }

    // Get ddb from registry
    this->downloadDdb(tagInfo.organization, tagInfo.dataset, tempDdbFolder.string());

    auto source = open(tempDdbFolder.string(), false);

    // Perform local diff using delta method using last stamp
    SyncManager sm(db.get());
    const auto delta = getDelta(source->getStamp(), sm.getLastStamp(tagInfo.registryUrl));
    LOGD << "Delta:";

    out << "Delta result: " << delta.adds.size() << " adds, "
        << delta.removes.size() << " removes" << std::endl;

    // Nothing to do? Early exit
    if (delta.adds.empty() && delta.removes.empty()){
        out << "Already up to date." << std::endl;
        db->close();
        source->close();
        io::assureIsRemoved(tempDdbFolder);
        return;
    }

    const auto tempNewFolder = tempDdbFolder / std::to_string(time(nullptr));

    // Let's download only if we have anything to download
    if (!delta.adds.empty()) {
        LOGD << "Temp new folder = " << tempNewFolder;

        const auto filesToDownload =
            boolinq::from(delta.adds)
                .where(
                    [](const AddAction &add) { return !add.isDirectory(); })
                .select([](const AddAction &add) { return add.path; })
                .toStdVector();

        // Download all the missing files
        this->downloadFiles(tagInfo.organization, tagInfo.dataset,
                            filesToDownload, tempNewFolder.string());

        // TODO: check current index to see if we already have
        // these files on disk (both path and hash) to avoid
        // downloading

        LOGD << "Files downloaded";
    } else {
        LOGD << "No files to download";
    }

    // Apply changes to local files
    try{
        applyDelta(delta, tempNewFolder, db.get(), mergeStrategy, out);
        io::assureIsRemoved(tempNewFolder);

        auto mPathList = delta.modifiedPathList();
        if (mPathList.size() > 0) syncLocalMTimes(db.get(), mPathList);

        // No errors? Update stamp
        sm.setLastStamp(tagInfo.registryUrl, source.get());
    }catch(const MergeException &e){
        out << "Found conflicts, but don't worry! Make a copy of the conflicting entries and use --keep-theirs or --keep-ours to finish the pull:" << std::endl << std::endl;

        for (auto &c : e.getConflicts()){
            out << "C\t" << c.path << " (" << c.description() << ")" <<  std::endl;
        }
    }

    db->close();
    source->close();

    // Cleanup
    io::assureIsRemoved(tempDdbFolder);
    io::assureIsRemoved(tempNewFolder);
}

void Registry::push(const std::string &path, const bool force,
                            std::ostream &out) {
    /*

    -- Push Workflow --

    1) Get our tag using tagmanager
    3) Get dataset mtime
    4) Alert if dataset_mtime > last_sync (it means we have less recent changes
    than server, so the push is pointless or potentially dangerous)
    5) Initialize server push
        5.1) Zip our ddb folder
        5.1) Call POST endpoint passing zip
        5.2) The server answers with the needed files list
    6) Foreach of the needed files call POST endpoint
    7) When done call commit endpoint
    8) Update last sync
    */

    auto db = open(path, true);

    TagManager tagManager(db.get());
    SyncManager syncManager(db.get());

    // 1) Get our tag using tagmanager
    const auto tag = tagManager.getTag();

    if (tag.empty()) throw IndexException("Cannot push if no tag is specified");

    LOGD << "Tag = " << tag;

    const auto tagInfo = RegistryUtils::parseTag(tag);

    try {
        // 3) Get dataset info
        const auto dsInfo = this->getDatasetInfo(tagInfo.organization, tagInfo.dataset);

        out << "Pushing to '" << tag << "'" << std::endl;
    } catch (RegistryNotFoundException &ex) {
        LOGD << "Dataset not found: " << ex.what();

        out << "Pushing to new '" << tag << "'" << std::endl;
    }

    // 5) Initialize server push
    LOGD << "Initializing server push";

    // 5.1) Call POST endpoint passing database stamp
    PushManager pushManager(this, tagInfo.organization, tagInfo.dataset);

    // 5.2) The server answers with the needed files list
    std::string registryStampChecksum = "";
    try{
        const auto regStamp = syncManager.getLastStamp(tagInfo.registryUrl);
        registryStampChecksum = regStamp["checksum"];
    }catch(const NoStampException &){
        // Nothing, this is the first time we push
    }

    const auto filesList = pushManager.init(registryStampChecksum, db->getStamp());

    LOGD << "Push initialized";
/*
    const auto basePath = ddbPath.parent_path();

    for (const auto &file : filesList) {
        const auto fullPath = basePath / file;

        out << "Transfering '" << file << "'" << std::endl;

        LOGD << "Upload: " << fullPath;

        // 6) Foreach of the needed files call POST endpoint
        pushManager.upload(fullPath.generic_string(), file);
    }

    out << "Transfers done" << std::endl;

    // 7) When done call commit endpoint
    pushManager.commit();

    LOGD << "Push committed, cleaning up";

    // Cleanup
    fs::remove(tempArchive);

    out << "Push complete" << std::endl; */
}

void Registry::handleError(net::Response &res) {
    if (res.hasData()) {
        LOGD << "Request error: " << res.getText();

        auto j = res.getJSON();
        if (j.contains("error"))
            throw RegistryException("Error response from registry: " +
                                    j["error"].get<std::string>());
        throw RegistryException("Invalid response from registry: " +
                                res.getText());
    }

    LOGD << "Request error: " << res.status();

    throw RegistryException(
        "Invalid response from registry. Returned status: " +
        std::to_string(res.status()));
}

}  // namespace ddb
