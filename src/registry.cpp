/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "registry.h"

#include <boolinq/boolinq.h>
#include <ddb.h>
#include <delta.h>
#include <mio.h>
#include <syncmanager.h>
#include <tagmanager.h>

#include "../vendor/miniz-cpp/zip_file.hpp"
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

DDB_DLL void Registry::ensureTokenValidity() {
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

DDB_DLL void Registry::clone(const std::string &organization,
                             const std::string &dataset,
                             const std::string &folder, std::ostream &out) {
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
        io::Path(fs::temp_directory_path() / std::to_string(time(nullptr)))
            .string() +
        ".tmp";

    LOGD << "Temp file = " << tempFile;

    auto start = std::chrono::system_clock::now();
    size_t prevBytes = 0;

    auto res = net::GET(downloadUrl)
                   .authCookie(this->authToken)
                   .verifySSL(false)
                   .progressCb([&start, &prevBytes, &out](size_t txBytes,
                                                          size_t totalBytes) {
                       if (txBytes == prevBytes) return true;

                       const auto now = std::chrono::system_clock::now();

                       const std::chrono::duration<double> dT = now - start;

                       if (dT.count() < 1) return true;

                       const auto dData = txBytes - prevBytes;
                       const auto speed = dData / dT.count();

                       out << "Downloading: " << io::bytesToHuman(txBytes)
                           << " @ " << io::bytesToHuman(speed) << "/s\t\t\r";
                       out.flush();

                       prevBytes = txBytes;
                       start = now;

                       return true;
                   })
                   .downloadToFile(tempFile);

    if (res.status() != 200) this->handleError(res);

    out << "Dataset downloaded (" << io::bytesToHuman(prevBytes) << ")\t\t"
        << std::endl;
    out << "Extracting to destination folder (this could take a while)"
        << std::endl;

    io::createDirectories(folder);

    try {
        miniz_cpp::zip_file file;

        file.load(tempFile);
        file.extractall(folder);
    } catch (const std::runtime_error &e) {
        LOGD << "Error extracting zip file";
        throw AppException(e.what());
    }

    std::filesystem::remove(tempFile);

    const auto ddbFolder = fs::path(folder) / DDB_FOLDER;

    SyncManager syncManager(ddbFolder);
    TagManager tagManager(ddbFolder);

    syncManager.setLastSync(this->url);
    tagManager.setTag(this->url + "/" + organization + "/" + dataset);

    out << "Done" << std::endl;
}

std::string Registry::getAuthToken() { return std::string(this->authToken); }

time_t Registry::getTokenExpiration() { return this->tokenExpiration; }

void to_json(json &j, const DatasetInfo &p) {
    j = json{{"path", p.path}, {"hash", p.hash},   {"type", p.type},
             {"size", p.size}, {"depth", p.depth}, {"mtime", p.mtime},
             {"meta", p.meta}};
}

void from_json(const json &j, DatasetInfo &p) {
    j.at("path").get_to(p.path);

    if (!j.at("hash").is_null()) j.at("hash").get_to(p.hash);

    j.at("type").get_to(p.type);
    j.at("size").get_to(p.size);
    j.at("depth").get_to(p.depth);
    j.at("mtime").get_to(p.mtime);

    // TODO: Add
    // j.at("meta").get_to(p.meta);
}

DDB_DLL DatasetInfo Registry::getDatasetInfo(const std::string &organization,
                                             const std::string &dataset) {
    this->ensureTokenValidity();

    const auto getUrl = url + "/orgs/" + organization + "/ds/" + dataset;

    LOGD << "Getting info of tag " << dataset << "/" << organization;

    auto res =
        net::GET(getUrl).authCookie(this->authToken).verifySSL(false).send();

    if (res.status() != 200) this->handleError(res);

    LOGD << "Data: " << res.getText();

    const auto j = res.getJSON();

    // try {
    // TODO: Catch errors
    const auto resArr = j.get<std::vector<DatasetInfo>>();

    if (resArr.empty())
        throw RegistryException("Invalid empty response from registry");

    return resArr[0];

    //} catch (std::runtime_error ex) {

    //    LOGD << "Exception: " << ex.what();

    //}
}

DDB_DLL void Registry::downloadDdb(const std::string &organization,
                                   const std::string &dataset,
                                   const std::string &folder) {
    this->ensureTokenValidity();

    const auto downloadUrl =
        url + "/orgs/" + organization + "/ds/" + dataset + "/ddb";

    LOGD << "Download url = " << downloadUrl;

    const auto tempFile =
        io::Path(fs::temp_directory_path() / std::to_string(time(nullptr)))
            .string() +
        ".tmp";

    LOGD << "Temp file = " << tempFile;

    auto res = net::GET(downloadUrl)
                   .authCookie(this->authToken)
                   .verifySSL(false)
                   .downloadToFile(tempFile);

    if (res.status() != 200) this->handleError(res);

    try {
        miniz_cpp::zip_file file;

        file.load(tempFile);
        file.extractall(folder);
    } catch (const std::runtime_error &e) {
        LOGD << "Error extracting zip file";
        throw AppException(e.what());
    }

    std::filesystem::remove(tempFile);

    LOGD << "Done";
}

DDB_DLL void Registry::downloadFiles(const std::string &organization,
                                     const std::string &dataset,
                                     const std::vector<std::string> &files,
                                     const std::string &folder) {
    this->ensureTokenValidity();

    auto downloadUrl =
        url + "/orgs/" + organization + "/ds/" + dataset + "/download";

    LOGD << "Download url = " << downloadUrl;

    const auto tempFile =
        io::Path(fs::temp_directory_path() / std::to_string(time(nullptr)))
            .string() +
        ".tmp";

    LOGD << "Temp file = " << tempFile;

    std::stringstream ss;
    for (const auto &file : files) ss << file << ',';

    auto paths = ss.str();

    // Remove last comma
    paths.pop_back();
    downloadUrl += "?path=" + paths;

    LOGD << "Paths = " << paths;

    auto res = net::GET(downloadUrl)
                   .authCookie(this->authToken)
                   .verifySSL(false)
                   //.multiPartFormData({"path", paths})
                   .downloadToFile(tempFile);

    if (res.status() != 200) this->handleError(res);

    LOGD << "Files archive downloaded, extracting";

    try {
        miniz_cpp::zip_file file;

        file.load(tempFile);
        file.extractall(folder);

        LOGD << "Archive extracted in " << folder;

        std::filesystem::remove(tempFile);

        LOGD << "Done";
    } catch (const std::runtime_error &e) {
        LOGD << "Error extracting zip file";
        throw AppException(e.what());
    }
}

DDB_DLL void ensureParentFolderExists(const fs::path &folder) {
    if (folder.has_parent_path()) {
        const auto parentPath = folder.parent_path();
        create_directories(parentPath);
    }
}

DDB_DLL void moveCopiesToTemp(const std::vector<CopyAction> &copies,
                              const fs::path &baseFolder,
                              const std::string &tempFolderName) {
    LOGD << "Moving copies to temp folder";

    for (auto copy : copies) {
        LOGD << copy.toString();

        const auto source = baseFolder / copy.source;
        const auto dest = baseFolder / tempFolderName / copy.source;

        LOGD << "SourcePath = " << source;
        LOGD << "DestPath = " << dest;

        ensureParentFolderExists(dest);

        const auto newPath = fs::path(tempFolderName) / copy.source;

        LOGD << "Changed copy path from " << copy.source << " to " << newPath;

        fs::copy(source, dest,
                 std::filesystem::copy_options::overwrite_existing);

        copy.source = newPath.string();
    }
}

const char *tmpFolderName = ".tmp";
const char *replaceSuffix = ".replace";

DDB_DLL void applyDelta(const Delta &res, const fs::path &destPath,
                        const fs::path &sourcePath) {
    const auto tempPath = destPath / tmpFolderName;
    create_directories(tempPath);

    moveCopiesToTemp(res.copies, destPath, tmpFolderName);

    LOGD << "Working on removes";

    for (const auto &rem : res.removes) {
        LOGD << rem.toString();

        const auto dest = destPath / rem.path;

        LOGD << "Dest = " << dest;

        if (rem.type != Directory) {
            if (exists(dest)) {
                LOGD << "File exists in dest, deleting it";
                fs::remove(dest);
            } else {
                LOGD << "File does not exist in dest, nothing to do";
            }
        } else {
            if (exists(dest)) {
                LOGD << "Directory exists in dest, deleting it";
                remove_all(dest);
            } else {
                LOGD << "Directory does not exist in dest, nothing to do";
            }
        }
    }

    LOGD << "Working on adds";

    for (const auto &add : res.adds) {
        LOGD << add.toString();

        const auto source = sourcePath / add.path;
        const auto dest = destPath / add.path;

        if (add.type != Directory) {
            LOGD << "Applying add by copying from source to dest";
            copy_file(source, dest,
                      std::filesystem::copy_options::overwrite_existing);
        } else {
            create_directories(dest);
        }
    }

    LOGD << "Working on direct copies";

    for (const auto &copy : res.copies) {
        LOGD << copy.toString();

        const auto source = destPath / copy.source;
        const auto dest = destPath / copy.destination;

        if (dest.has_parent_path()) {
            const auto destFolder = dest.parent_path();
            create_directories(destFolder);
        }

        if (exists(dest)) {
            const auto newPath = fs::path(dest.string() + replaceSuffix);
            LOGD << "Dest file exists, writing shadow";
            copy_file(source, newPath);
        } else {
            LOGD << "Dest file does not exist, performing copy";
            copy_file(source, dest);
        }
    }

    LOGD << "Working on shadow copies";

    for (const auto &copy : res.copies) {
        const auto dest = destPath / copy.destination;
        const auto destShadow = fs::path(dest.string() + ".replace");

        if (exists(destShadow)) {
            LOGD << copy.toString();
            LOGD << "Shadow file exists, replacing original one";

            // Why don't we have fs::move??
            std::filesystem::copy(
                destShadow, dest,
                std::filesystem::copy_options::overwrite_existing);
            std::filesystem::remove(destShadow);
        }
    }

    if (exists(tempPath)) remove_all(tempPath);
}

DDB_DLL void Registry::pull(const std::string &path, const bool force,
                            std::ostream &out) {
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

    out << "Pull from " << this->url << std::endl;
    LOGD << "Pull from " << this->url;

    // 2) Get our last sync time for that specific registry using syncmanager

    // const auto currentPath = std::filesystem::current_path();
    auto db = open(path, true);
    const auto ddbPath = fs::path(db->getOpenFile()).parent_path();

    LOGD << "Ddb folder = " << ddbPath;

    TagManager tagManager(ddbPath);
    SyncManager syncManager(ddbPath);

    // 1) Get our tag using tagmanager
    const auto tag = tagManager.getTag();

    // 2) Get our last sync time for that specific registry using syncmanager
    const auto lastSync = syncManager.getLastSync(this->url);

    LOGD << "Tag = " << tag;
    LOGD << "LastSync = " << lastSync;

    const auto tagInfo = RegistryUtils::parseTag(tag);

    // 3) Get dataset mtime
    const auto dsInfo =
        this->getDatasetInfo(tagInfo.organization, tagInfo.dataset);

    LOGD << "Dataset mtime = " << dsInfo.mtime;

    out << "Using tag '" << tag << "', last sync " << lastSync << ", dataset mtime " << dsInfo.mtime << std::endl;
    
    // 4) Alert if dataset_mtime < last_sync (it means we have more recent
    // changes than server, so the pull is pointless or potentially dangerous)
    if (dsInfo.mtime < lastSync && !force)
        throw AppException(
            "Can pull only if dataset changes are newer than ours. Use force "
            "parameter to override (CAUTION)");

    const auto tempDdbFolder = fs::temp_directory_path() / "ddb_temp_folder" /
                               (tagInfo.organization + "-" + tagInfo.dataset);

    LOGD << "Temp ddb folder = " << tempDdbFolder;

    // 5) Get ddb from registry
    this->downloadDdb(tagInfo.organization, tagInfo.dataset,
                      tempDdbFolder.string());

    out << "Remote ddb downloaded";

    LOGD << "Remote ddb downloaded";

    auto source = open(tempDdbFolder.string(), true);

    // 6) Perform local diff using delta method
    const auto delta = getDelta(source.get(), db.get());

    LOGD << "Delta:";

    json j = delta;
    LOGD << j.dump();

    out << "Delta result: " << delta.adds.size() << " adds, " << delta.copies.size() << " copies" << delta.removes.size() << " removes";
    
    const auto tempNewFolder = fs::temp_directory_path() / "ddb_new_folder" /
                               (tagInfo.organization + "-" + tagInfo.dataset);

    // Let's download only if we have anything to download
    if (!delta.adds.empty()) {
        LOGD << "Temp new folder = " << tempNewFolder;

        const auto filesToDownload =
            boolinq::from(delta.adds)
                .where(
                    [](const AddAction &add) { return add.type != Directory; })
                .select([](const AddAction &add) { return add.path; })
                .toStdVector();

        out << "Downloading missing files (this could take a while)";

        LOGD << "Files to download:";
        j = filesToDownload;
        LOGD << j.dump();

        // 7) Download all the missing files
        this->downloadFiles(tagInfo.organization, tagInfo.dataset,
                            filesToDownload, tempNewFolder.string());

        LOGD << "Files downloaded, applying delta";
    } else {
        LOGD << "No files to download";

        // Check if we have anything to do
        if (delta.copies.empty() && delta.removes.empty()) {
            LOGD << "No changes to perform, pull done";
            out << "No changes, nothing to do here";
            // NOTE: Should be update lastsync?

            return;
        }
    }

    // 8) Apply changes to local files
    applyDelta(delta, ddbPath.parent_path(), tempNewFolder);

    out << "Delta applied";

    LOGD << "Replacing ddb folder";

    // 9) Replace ddb database
    copy(tempDdbFolder, ddbPath);

    out << "DDB replaced";

    LOGD << "Updating syncmanager and tagmanager";

    // 10) Update last sync time
    syncManager.setLastSync(this->url);
    tagManager.setTag(tag);

    out << "Updated last sync time, pull done";

    LOGD << "Pull done";
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
