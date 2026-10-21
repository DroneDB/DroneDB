/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "registry.h"

#include <boolinq/boolinq.h>

#include <algorithm>

#include <cpr/cpr.h>

#include "build.h"
#include "ddb.h"
#include "delta.h"
#include "exceptions.h"
#include "hash.h"
#include "json.h"
#include "registryutils.h"
#include "logger.h"
#include "mio.h"
#include "mzip.h"
#include "parallel.h"
#include "pushmanager.h"
#include "raster_cache.h"
#include "syncmanager.h"
#include "tagmanager.h"
#include "url.h"
#include "userprofile.h"
#include "utils.h"

using homer6::Url;

namespace ddb {

Registry::Registry(const std::string& url, bool sslVerify) {
    this->sslVerify = sslVerify;
    std::string urlStr = url;

    if (urlStr.empty())
        urlStr = std::string(DEFAULT_REGISTRY);

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

    const std::string port =
        u.getPort() != 80 && u.getPort() != 443 ? ":" + std::to_string(u.getPort()) : "";
    this->url = u.getScheme() + "://" + u.getHost() + port + u.getPath();

    // LOGD << "Registry URL: " << this->url;
}

std::string Registry::getUrl(const std::string& path) const {
    return url + path;
}

std::string Registry::login() {
    const auto ac = UserProfile::get()->getAuthManager()->loadCredentials(this->url);

    if (ac.empty())
        throw InvalidArgsException("No stored credentials for registry at '" + this->url + "'");

    return login(ac.username, ac.password);
}

std::string Registry::login(const std::string& username, const std::string& password) {
    auto res = cpr::Post(cpr::Url(this->getUrl("/users/authenticate")),
                         cpr::Payload{{"username", username}, {"password", password}},
                         cpr::VerifySsl(this->sslVerify));

    if (res.status_code == 200) {
        const json j = json::parse(res.text);
        const auto token = j["token"].get<std::string>();
        const auto expiration = j["expires"].get<time_t>();

        // Save for next time
        UserProfile::get()->getAuthManager()->saveCredentials(url,
                                                              AuthCredentials(username, password));

        this->authToken = token;
        this->tokenExpiration = expiration;

        // LOGD << "AuthToken = " << token;
        // LOGD << "Expiration = " << expiration;

        return std::string(token);
    }
    if (const auto message = RegistryUtils::errorMessage(res.text); !message.empty())
        throw AuthException("Login failed: " + message);

    throw AuthException("Login failed: host returned " + std::to_string(res.status_code));
}

void Registry::ensureTokenValidity() {
    if (this->authToken.empty())
        return;  // No credentials saved

    const auto now = std::time(nullptr);

    // LOGD << "Now = " << now << ", expration = " << this->tokenExpiration


    // If the token is still valid we have nothing to do
    if (now < this->tokenExpiration) {
        // LOGD << "Token still valid";
        return;
    }

    // LOGD << "Token expired: re-login";
    // Otherwise login
    this->login();
}

bool Registry::logout() {
    return UserProfile::get()->getAuthManager()->deleteCredentials(url);
}

void Registry::clone(const std::string& organization,
                     const std::string& dataset,
                     const std::string& folder,
                     std::ostream& out) {
    if (fs::exists(folder) && !fs::is_empty(folder)) {
        throw FSException(folder + " already exists");
    }

    io::assureFolderExists(folder);

    this->ensureTokenValidity();

    initIndex(folder);
    const auto db = ddb::open(folder, false);

    TagManager tagManager(db.get());
    tagManager.setTag(this->url + "/" + organization + "/" + dataset);
    this->pull(folder, MergeStrategy::KeepTheirs, out);
}

std::string Registry::getAuthToken() {
    return std::string(this->authToken);
}

time_t Registry::getTokenExpiration() {
    return this->tokenExpiration;
}

Entry Registry::getDatasetInfo(const std::string& organization, const std::string& dataset) {
    this->ensureTokenValidity();

    const auto getUrl = url + "/orgs/" + organization + "/ds/" + dataset;

    // LOGD << "Getting info of tag " << dataset << "/" << organization;

    auto res = cpr::Get(cpr::Url(getUrl),
                        utils::authCookie(this->authToken),
                        cpr::VerifySsl(this->sslVerify));

    if (res.status_code == 404)
        throw RegistryNotFoundException("Dataset not found");

    if (res.status_code != 200)
        this->handleError(res);

    // LOGD << "Data: " << res.text;

    const json j = json::parse(res.text);

    if (j.empty())
        throw RegistryException("Invalid empty response from registry");

    Entry e;
    e.fromJSON(j[0]);
    return e;
}

void Registry::downloadDdb(const std::string& organization,
                           const std::string& dataset,
                           const std::string& folder) {
    this->ensureTokenValidity();
    const auto downloadUrl = url + "/orgs/" + organization + "/ds/" + dataset + "/ddb";

    auto res = cpr::Get(cpr::Url(downloadUrl),
                        utils::authCookie(this->authToken),
                        cpr::VerifySsl(this->sslVerify));

    // 1. Controllo errori di rete/SSL prima di qualsiasi accesso al payload
    if (res.error.code != cpr::ErrorCode::OK) {
        this->handleError(res);
        return;
    }

    // 2. Controllo HTTP status code
    if (res.status_code != 200) {
        this->handleError(res);
        return;
    }

    // 3. Estrazione ZIP con buffer binario sicuro
    zip::extractAllFromBuffer(res.text.c_str(), res.text.size(), folder);
}

json Registry::getStamp(const std::string& organization, const std::string& dataset) {
    this->ensureTokenValidity();
    const auto stampUrl = url + "/orgs/" + organization + "/ds/" + dataset + "/stamp";

    auto res = cpr::Get(cpr::Url(stampUrl),
                        utils::authCookie(this->authToken),
                        cpr::VerifySsl(this->sslVerify));

    if (res.status_code != 200)
        this->handleError(res);

    // Quick sanity check (a discarded parse is not an object)
    json stamp = json::parse(res.text, nullptr, false);
    if (stamp.is_object() && stamp.contains("checksum")) {
        return stamp;
    } else {
        throw RegistryException("Invalid stamp: " + res.text);
    }
}

void Registry::downloadFiles(const std::string& organization,
                             const std::string& dataset,
                             const std::vector<std::string>& files,
                             const std::string& folder,
                             std::ostream& out) {
    if (files.empty()) {
        // LOGD << "Asked to download an empty list of files...";
        return;
    }

    this->ensureTokenValidity();

    auto downloadUrl = url + "/orgs/" + organization + "/ds/" + dataset + "/download";

    // LOGD << "Download url = " << downloadUrl;

    auto start = std::chrono::system_clock::now();
    size_t prevBytes = 0;

    auto progressCb =
        ([&start, &prevBytes, &out](size_t, size_t txBytes, size_t, size_t, intptr_t) -> bool {
            if (txBytes == prevBytes)
                return true;

            const auto now = std::chrono::system_clock::now();

            const std::chrono::duration<double> dT = now - start;

            if (dT.count() < 1)
                return true;

            const auto dData = txBytes - prevBytes;
            const auto speed = dData / dT.count();

            out << "Downloading " << io::bytesToHuman(txBytes) << " @ " << io::bytesToHuman(speed)
                << "/s\t\t\r";
            out.flush();

            prevBytes = txBytes;
            start = now;

            return true;
        });

    if (files.size() == 1) {
        downloadUrl += "?path=" + files[0];

        const auto destPath = fs::path(folder) / files[0];

        io::createDirectories(destPath.parent_path());

        std::ofstream of(destPath, std::ios::binary);
        auto res = cpr::Download(of,
                                 cpr::Url(downloadUrl),
                                 utils::authCookie(this->authToken),
                                 cpr::ProgressCallback(progressCb),
                                 cpr::VerifySsl(this->sslVerify));

        if (res.status_code != 200)
            this->handleError(res);
    } else {
        const auto tempFile = (fs::path(folder) / (utils::generateRandomString(8) + ".tmp"));
        io::assureFolderExists(tempFile.parent_path());

        // Joins path list
        const auto paths = utils::join(files);

        // LOGD << "Paths = " << paths;

        std::ofstream of(tempFile, std::ios::binary);

        auto res =
            cpr::Post(cpr::Url(downloadUrl),
                      utils::authCookie(this->authToken),
                      cpr::Payload{{"path", paths}},
                      cpr::WriteCallback([&of](const std::string_view& data, intptr_t) -> bool {
                          of.write(data.data(), data.size());
                          return true;
                      }),
                      cpr::VerifySsl(this->sslVerify));

        of.close();

        if (res.status_code != 200)
            this->handleError(res);

        // LOGD << "Files archive downloaded, extracting";

        try {
            zip::extractAll(tempFile.string(), folder);
            io::assureIsRemoved(tempFile);

            // LOGD << "Done";
        } catch (const std::runtime_error& e) {
            io::assureIsRemoved(tempFile);
            throw AppException(e.what());
        }
    }
}

json Registry::getMetaDump(const std::string& organization,
                           const std::string& dataset,
                           const std::vector<std::string>& ids) {
    this->ensureTokenValidity();
    const auto metaDumpUrl = url + "/orgs/" + organization + "/ds/" + dataset + "/meta/dump";

    auto res = cpr::Post(cpr::Url(metaDumpUrl),
                         utils::authCookie(this->authToken),
                         cpr::Payload{{"ids", json(ids).dump()}},
                         cpr::VerifySsl(this->sslVerify));

    if (res.status_code != 200)
        this->handleError(res);

    // Quick sanity check (a discarded parse is not an array)
    json metaDump = json::parse(res.text, nullptr, false);
    if (metaDump.is_array()) {
        return metaDump;
    } else {
        throw RegistryException("Invalid meta dump: " + res.text);
    }
}

void ensureParentFolderExists(const fs::path& folder) {
    if (folder.has_parent_path()) {
        const auto parentPath = folder.parent_path();
        io::assureFolderExists(parentPath);
    }
}

namespace {

// Checks that every file add in `sourcePath` has the hash the delta declares. Runs before
// applyDelta changes anything, on the I/O threads (ioThreads: the reads dominate), with no
// transaction open.
void verifyDeltaHashes(const Delta& d, const fs::path& sourcePath) {
    std::vector<const AddAction*> files;
    for (const auto& add : d.adds)
        if (!add.isDirectory())
            files.push_back(&add);
    if (files.empty())
        return;

    parallel::forEachIndexIo(files.size(), [&](size_t i) {
        const auto& add = *files[i];
        const auto actual = Hash::fileSHA256((sourcePath / add.path).string());
        if (actual != add.hash)
            throw AppException("Hash mismatch for " + add.path + ": expected " + add.hash + ", got " + actual);
    });
}

// A delta path must name something inside the dataset
void checkDeltaPaths(const Delta& d) {
    const auto check = [](const std::string& path) {
        const fs::path p(path);
        const bool escapes = std::any_of(p.begin(), p.end(), [](const fs::path& part) { return part == ".."; });
        if (p.is_absolute() || p.has_root_name() || p.has_root_directory() || escapes)
            throw InvalidArgsException("Invalid path in delta: " + path);
    };
    for (const auto& add : d.adds)
        check(add.path);
    for (const auto& rem : d.removes)
        check(rem.path);
}

// Whether a delta entry can be applied over the destination's version of `path`: there is
// none, it already matches, or it did not change since the stamp the delta was computed
// from (`baseHash`). Otherwise both sides changed it: the merge strategy decides.
bool canApply(Database* destination,
              const std::string& path,
              const std::string& hash,
              const std::optional<std::string>& baseHash,
              const MergeStrategy mergeStrategy,
              const ConflictType conflictType,
              std::vector<Conflict>& conflicts,
              bool& indexed) {
    Entry e;
    indexed = getEntry(destination, path, e);
    if (!indexed || hash == e.hash || (baseHash && *baseHash == e.hash))
        return true;

    switch (mergeStrategy) {
        case MergeStrategy::DontMerge:
            conflicts.push_back(Conflict(path, conflictType));
            return false;
        case MergeStrategy::KeepOurs:
            return false;
        case MergeStrategy::KeepTheirs:
        default:
            return true;
    }
}

}  // namespace

std::vector<Conflict> applyDelta(const Delta& d,
                                 const fs::path& sourcePath,
                                 Database* destination,
                                 const MergeStrategy mergeStrategy,
                                 const json& sourceMetaDump,
                                 std::ostream& out,
                                 bool verifyHashes) {
    std::vector<Conflict> conflicts;

    // File operations
    if (d.adds.size() > 0 || d.removes.size() > 0) {
        const fs::path destPath = destination->rootDirectory();

        checkDeltaPaths(d);
        if (verifyHashes)
            verifyDeltaHashes(d, sourcePath);

        // Removes. We don't check whether the file has been modified on the FS, perhaps we should?
        std::vector<std::string> indexedRemovals;
        std::vector<const RemoveAction*> fileRemovals;
        for (const auto& rem : d.removes) {
            // The dataset root (older versions indexed it as "") is never removed
            if (rem.path.empty())
                continue;

            // rem.hash is the hash in the destination stamp
            bool indexed;
            if (!canApply(destination, rem.path, rem.hash, rem.hash, mergeStrategy,
                          ConflictType::RemoteDeleteLocalModified, conflicts, indexed))
                continue;

            const auto dest = destPath / rem.path;
            if (fs::exists(dest)) {
                if (indexed)
                    indexedRemovals.push_back(dest.string());
                fileRemovals.push_back(&rem);
            }
        }

        // Children before their folder even when the delta is not sorted: removing a folder
        // removes its content from the index
        std::sort(indexedRemovals.rbegin(), indexedRemovals.rend());
        std::sort(fileRemovals.begin(), fileRemovals.end(),
                  [](const RemoveAction* l, const RemoveAction* r) { return l->path > r->path; });

        removeFromIndex(destination, indexedRemovals);
        for (const auto* rem : fileRemovals) {
            // A raster kept open by recent tile requests would block this on Windows
            releaseCachedFiles(destPath / rem->path);
            io::assureIsRemoved(destPath / rem->path);
            out << "D\t" << rem->path << std::endl;
        }

        const auto indexAdded = [&](const std::vector<KnownFile>& files) {
            addToIndexWithHashes(destination, files, [&out](const Entry& e, bool updated) {
                out << (updated ? "U" : "A") << "\t" << e.path << std::endl;
                return true;
            });
        };

        // Adds (sorted by path: folders before their content). Files are moved, not
        // copied: the source folder is a temporary folder on the same volume.
        std::vector<KnownFile> addedFiles;
        try {
            for (const auto& add : d.adds) {
                if (add.path.empty())
                    continue;

                bool indexed;
                if (!canApply(destination, add.path, add.hash, add.baseHash, mergeStrategy,
                              ConflictType::BothModified, conflicts, indexed))
                    continue;

                const auto dest = destPath / add.path;
                if (add.isDirectory()) {
                    io::createDirectories(dest);
                } else {
                    releaseCachedFiles(dest);
                    io::moveOrCopy(sourcePath / add.path, dest);
                }

                addedFiles.push_back({dest.string(), add.hash});
            }
        } catch (const std::exception& e) {
            // The files already moved are gone from the source folder: index them, so the
            // index matches the dataset and a new delta does not ask for them again
            LOGW << "Delta apply failed after " << addedFiles.size() << " adds: " << e.what();
            try {
                indexAdded(addedFiles);
            } catch (const std::exception& ie) {
                LOGW << "Cannot index the adds applied before the failure: " << ie.what();
            }
            throw;
        }

        indexAdded(addedFiles);

        if (conflicts.size() == 0) {
            auto mPathList = d.modifiedPathList();
            if (mPathList.size() > 0)
                syncLocalMTimes(destination, mPathList);
        }
    }

    // Early exit in case of conflicts
    if (conflicts.size() > 0)
        return conflicts;

    // Meta operations
    if (d.metaAdds.size() > 0) {
        json metaRestore = json::array();

        std::unordered_map<std::string, bool> metaIds;
        for (auto& id : d.metaAdds)
            metaIds[id] = true;

        for (auto& meta : sourceMetaDump) {
            if (!meta.contains("id"))
                throw InvalidArgsException("Invalid meta element: " + meta.dump());
            if (metaIds.find(meta["id"]) != metaIds.end())
                metaRestore.push_back(meta);
        }

        destination->getMetaManager()->restore(metaRestore);
    }

    if (d.metaRemoves.size() > 0) {
        destination->getMetaManager()->bulkRemove(d.metaRemoves);
    }

    return conflicts;
}

void Registry::pull(const std::string& path, const MergeStrategy mergeStrategy, std::ostream& out) {
    // LOGD << "Pull from " << this->url;

    auto db = open(path, true);
    TagManager tagManager(db.get());

    // Get our tag using tagmanager
    const auto tag = tagManager.getTag();

    if (tag.empty())
        throw IndexException("Cannot pull if no tag is specified");

    // LOGD << "Tag = " << tag;

    const auto tagInfo = RegistryUtils::parseTag(tag);

    out << "Pulling from '" << tag << "'" << std::endl;

    const auto tempDdbFolder =
        db->ddbDirectory() / "tmp" / "pull_cache" / (tagInfo.organization + "-" + tagInfo.dataset);

    if (fs::exists(tempDdbFolder)) {
        // TODO: there might be ways to resume downloads if a user CTRL+Cs
        io::assureIsRemoved(tempDdbFolder);
    }

    // Get stamp from registry
    json remoteStamp = this->getStamp(tagInfo.organization, tagInfo.dataset);

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
        remoteMetaDump = this->getMetaDump(tagInfo.organization, tagInfo.dataset, delta.metaAdds);
    }

    const auto tempNewFolder = tempDdbFolder / std::to_string(time(nullptr));

    // Let's download only if we have anything to download
    if (!delta.adds.empty()) {
        // LOGD << "Temp new folder = " << tempNewFolder;

        auto localMap = computeDeltaLocals(delta, db.get(), tempNewFolder.string());

        const auto filesToDownload =
            boolinq::from(delta.adds)
                .where([&localMap](const AddAction& add) {
                    return !add.isDirectory() && localMap.find(add.hash) == localMap.end();
                })
                .select([](const AddAction& add) { return add.path; })
                .toStdVector();

        // Download all the missing files
        this->downloadFiles(tagInfo.organization,
                            tagInfo.dataset,
                            filesToDownload,
                            tempNewFolder.string(),
                            out);

        // LOGD << "Files downloaded";
    } else {
        // LOGD << "No files to download";
    }

    // TODO: speedup: we should be able to check for conflicts
    // before we download the files

    // Apply changes to local files
    auto conflicts = applyDelta(delta, tempNewFolder, db.get(), mergeStrategy, remoteMetaDump, out);
    io::assureIsRemoved(tempNewFolder);

    if (conflicts.size() == 0) {
        // No errors? Update stamp
        sm.setLastStamp(tagInfo.registryUrl, remoteStamp);
    } else {
        out << "Found conflicts, but don't worry! Make a copy of the conflicting entries and use "
               "--keep-theirs or --keep-ours to finish the pull:"
            << std::endl
            << std::endl;

        for (auto& c : conflicts) {
            out << "C\t" << c.path << " (" << c.description() << ")" << std::endl;
        }
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

std::unordered_map<std::string, bool> computeDeltaLocals(Delta d,
                                                         Database* db,
                                                         const std::string& hlDestFolder) {
    // Do we have files locally? If so we don't need to
    // download them, we just create hard links (or copies) to it after
    // validating that they are indeed the same
    // This function creates hard links only if hlDestFolder is set
    // Otherwise it just returns the map of valid local hashes
    const auto q = db->query("SELECT path,mtime FROM entries WHERE hash = ?");
    std::unordered_map<std::string, bool> localMap;

    std::unordered_map<std::string, bool> addsMap;
    for (auto& add : d.adds) {
        if (add.hash.empty())
            continue;
        addsMap[add.path] = true;
    }

    for (auto& add : d.adds) {
        if (add.hash.empty())
            continue;

        q->bind(1, add.hash);
        if (q->fetch()) {
            auto ePath = q->getText(0);
            // Check the filesystem to make sure this hasn't been modified
            io::Path p(db->rootDirectory() / ePath);
            const double eMtime = q->getDouble(1);
            bool valid = false;
            if (io::sameModifiedTime(eMtime, p.getModifiedTimePrecise())) {
                valid = true;
            } else {
                // Actually compute hash
                valid = Hash::fileSHA256(p.get().string()) == add.hash;
            }

            if (valid) {
                if (!hlDestFolder.empty()) {
                    const auto destPath = fs::path(hlDestFolder) / add.path;
                    io::createDirectories(destPath.parent_path());

                    // We can leverage hard links ONLY
                    // if the path we are linking is not itself
                    // part of an add operation (otherwise it could be
                    // overwritten)
                    if (addsMap.find(ePath) == addsMap.end())
                        io::hardlinkSafe(p.get(), destPath);
                    else
                        io::copy(p.get(), destPath);
                }
                localMap[add.hash] = true;
            }
        }
        q->reset();
    }

    return localMap;
}

void Registry::push(const std::string& path, std::ostream& out) {
    auto db = open(path, true);

    TagManager tagManager(db.get());
    SyncManager syncManager(db.get());

    // Get our tag using tagmanager
    const auto tag = tagManager.getTag();

    if (tag.empty())
        throw IndexException("Cannot push if no tag is specified");

    // LOGD << "Tag = " << tag;

    const auto tagInfo = RegistryUtils::parseTag(tag);

    try {
        // 3) Get dataset info
        const auto dsInfo = this->getDatasetInfo(tagInfo.organization, tagInfo.dataset);

        out << "Pushing to '" << tag << "'" << std::endl;
    } catch (RegistryNotFoundException& ex) {
        // LOGD << "Dataset not found: " << ex.what();

        out << "Pushing to new '" << tag << "'" << std::endl;
    }

    // Initialize server push
    // LOGD << "Initializing server push";

    // Call POST endpoint passing database stamp
    PushManager pushManager(this, tagInfo.organization, tagInfo.dataset);

    std::string registryStampChecksum = "";
    try {
        const auto regStamp = syncManager.getLastStamp(tagInfo.registryUrl);
        registryStampChecksum = regStamp["checksum"];
    } catch (const NoStampException&) {
        // Nothing, this is the first time we push
    }

    const auto pir = pushManager.init(registryStampChecksum, db->getStamp());

    // LOGD << "Push initialized";

    // Push meta
    if (pir.neededMeta.size() > 0) {
        out << "Transferring metadata (" << pir.neededMeta.size() << ")" << std::endl;
        pushManager.meta(db->getMetaManager()->dump(pir.neededMeta), pir.token);
    }

    const auto basePath = db->rootDirectory();

    for (const auto& file : pir.neededFiles) {
        const auto fullPath = basePath / file;

        out << "Transfering '" << file << "'" << std::endl;

        // Foreach of the needed files call POST endpoint
        pushManager.upload(fullPath.generic_string(), file, pir.token);
    }

    // When done call commit endpoint
    pushManager.commit(pir.token);

    // Update stamp
    syncManager.setLastStamp(tagInfo.registryUrl, db.get());

    out << "Push completed" << std::endl;
}

void Registry::handleError(cpr::Response& res) {
    if (res.status_code == 401)
        throw AuthException("Unauthorized");

    if (res.status_code != 200) {
        // LOGD << "Request error: " << res.text;

        if (const auto message = RegistryUtils::errorMessage(res.text); !message.empty())
            throw RegistryException("Error response from registry: " + message);
        throw RegistryException("Invalid response from registry: " + res.text);
    }

    // LOGD << "Request error: " << res.error.code << " -> " << res.error.message;

    throw RegistryException("Invalid response from registry. Returned status: " +
                            std::to_string(res.status_code));
}

}  // namespace ddb
