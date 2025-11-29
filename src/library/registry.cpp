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
#include <cpr/cpr.h>
#include "url.h"
#include "userprofile.h"
#include "utils.h"

using homer6::Url;

namespace ddb
{

    Registry::Registry(const std::string &url, bool sslVerify)
    {
        this->sslVerify = sslVerify;
        std::string urlStr = url;

        if (urlStr.empty())
            urlStr = std::string(DEFAULT_REGISTRY);

        Url u;

        // Always append https if no protocol is specified
        if (urlStr.find("https://") != 0 && urlStr.find("http://") != 0)
        {
            u.fromString("https://" + urlStr);
        }
        else
        {
            u.fromString(urlStr);
        }

        // Validate and set URL
        if (u.getScheme() != "https" && u.getScheme() != "http")
        {
            throw URLException("Registry URL can only be http/https");
        }

        const std::string port = u.getPort() != 80 && u.getPort() != 443
                                     ? ":" + std::to_string(u.getPort())
                                     : "";
        this->url = u.getScheme() + "://" + u.getHost() + port + u.getPath();

        // LOGD << "Registry URL: " << this->url;
    }

    std::string Registry::getUrl(const std::string &path) const
    {
        return url + path;
    }

    std::string Registry::login()
    {
        const auto ac =
            UserProfile::get()->getAuthManager()->loadCredentials(this->url);

        if (ac.empty())
            throw InvalidArgsException("No stored credentials for registry at '" +
                                       this->url + "'");

        return login(ac.username, ac.password);
    }

    std::string Registry::login(const std::string &username,
                                const std::string &password)
    {

        auto res = cpr::Post(cpr::Url(this->getUrl("/users/authenticate")),
                             cpr::Payload{{"username", username}, {"password", password}},
                             cpr::VerifySsl(this->sslVerify));

        json j = res.text;

        if (res.status_code == 200)
        {
            const auto token = j["token"].get<std::string>();
            const auto expiration = j["expires"].get<time_t>();

            // Save for next time
            UserProfile::get()->getAuthManager()->saveCredentials(
                url, AuthCredentials(username, password));

            this->authToken = token;
            this->tokenExpiration = expiration;

            // LOGD << "AuthToken = " << token;
            // LOGD << "Expiration = " << expiration;

            return std::string(token);
        }
        if (j.contains("error"))
        {
            throw AuthException("Login failed: " + j["error"].get<std::string>());
        }

        throw AuthException("Login failed: host returned " +
                            std::to_string(res.status_code));
    }

    void Registry::ensureTokenValidity()
    {
        if (this->authToken.empty())
            return; // No credentials saved

        const auto now = std::time(nullptr);

        // LOGD << "Now = " << now << ", expration = " << this->tokenExpiration


        // If the token is still valid we have nothing to do
        if (now < this->tokenExpiration)
        {
            // LOGD << "Token still valid";
            return;
        }

        // LOGD << "Token expired: re-login";
        // Otherwise login
        this->login();
    }

    bool Registry::logout()
    {
        return UserProfile::get()->getAuthManager()->deleteCredentials(url);
    }

    void Registry::clone(const std::string &organization,
                         const std::string &dataset,
                         const std::string &folder, std::ostream &out)
    {
        if (fs::exists(folder) && !fs::is_empty(folder))
        {
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

    std::string Registry::getAuthToken() { return std::string(this->authToken); }

    time_t Registry::getTokenExpiration() { return this->tokenExpiration; }

    Entry Registry::getDatasetInfo(const std::string &organization,
                                   const std::string &dataset)
    {
        this->ensureTokenValidity();

        const auto getUrl = url + "/orgs/" + organization + "/ds/" + dataset;

        // LOGD << "Getting info of tag " << dataset << "/" << organization;

        auto res = cpr::Get(cpr::Url(getUrl), utils::authCookie(this->authToken),
                            cpr::VerifySsl(this->sslVerify));

        if (res.status_code == 404)
            throw RegistryNotFoundException("Dataset not found");

        if (res.status_code != 200)
            this->handleError(res);

        // LOGD << "Data: " << res.text;

        const json j = res.text;

        if (j.empty())
            throw RegistryException("Invalid empty response from registry");

        Entry e;
        e.fromJSON(j[0]);
        return e;
    }

    void Registry::downloadDdb(const std::string &organization,
                               const std::string &dataset,
                               const std::string &folder)
    {
        this->ensureTokenValidity();

        const auto downloadUrl =
            url + "/orgs/" + organization + "/ds/" + dataset + "/ddb";

        // LOGD << "Download url = " << downloadUrl;

        // char *buffer;
        size_t length;

        auto res = cpr::Get(cpr::Url(downloadUrl), utils::authCookie(this->authToken),
                            cpr::VerifySsl(this->sslVerify));

        if (res.status_code != 200)
            this->handleError(res);

        zip::extractAllFromBuffer(res.text.c_str(), length, folder);

        // LOGD << "Done";
    }

    json Registry::getStamp(const std::string &organization, const std::string &dataset)
    {
        this->ensureTokenValidity();
        const auto stampUrl = url + "/orgs/" + organization + "/ds/" + dataset + "/stamp";

        auto res = cpr::Get(cpr::Url(stampUrl), utils::authCookie(this->authToken),
                            cpr::VerifySsl(this->sslVerify));

        if (res.status_code != 200)
            this->handleError(res);

        json stamp = res.text;

        // Quick sanity check<< ", diff = " << now - this->tokenExpiration;
        if (stamp.contains("checksum"))
        {
            return stamp;
        }
        else
        {
            throw RegistryException("Invalid stamp: " + stamp.dump());
        }
    }

    void Registry::downloadFiles(const std::string &organization,
                                 const std::string &dataset,
                                 const std::vector<std::string> &files,
                                 const std::string &folder,
                                 std::ostream &out)
    {
        if (files.empty())
        {
            // LOGD << "Asked to download an empty list of files...";
            return;
        }

        this->ensureTokenValidity();

        auto downloadUrl =
            url + "/orgs/" + organization + "/ds/" + dataset + "/download";

        // LOGD << "Download url = " << downloadUrl;

        auto start = std::chrono::system_clock::now();
        size_t prevBytes = 0;

        auto progressCb = ([&start, &prevBytes, &out](size_t, size_t txBytes, size_t, size_t, intptr_t) -> bool
                           {
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

            return true; });

        if (files.size() == 1)
        {
            downloadUrl += "?path=" + files[0];

            const auto destPath = fs::path(folder) / files[0];

            io::createDirectories(destPath.parent_path());

            std::ofstream of(destPath, std::ios::binary);
            auto res = cpr::Download(of, cpr::Url(downloadUrl), utils::authCookie(this->authToken),
                                     cpr::ProgressCallback(progressCb), cpr::VerifySsl(this->sslVerify));

            if (res.status_code != 200)
                this->handleError(res);
        }
        else
        {
            const auto tempFile = (fs::path(folder) / (utils::generateRandomString(8) + ".tmp"));
            io::assureFolderExists(tempFile.parent_path());

            // Joins path list
            const auto paths = utils::join(files);

            // LOGD << "Paths = " << paths;

            std::ofstream of(tempFile, std::ios::binary);

            auto res = cpr::Post(cpr::Url(downloadUrl),
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

            try
            {
                zip::extractAll(tempFile.string(), folder);
                io::assureIsRemoved(tempFile);

                // LOGD << "Done";
            }
            catch (const std::runtime_error &e)
            {
                io::assureIsRemoved(tempFile);
                throw AppException(e.what());
            }
        }
    }

    json Registry::getMetaDump(const std::string &organization, const std::string &dataset, const std::vector<std::string> &ids)
    {
        this->ensureTokenValidity();
        const auto metaDumpUrl = url + "/orgs/" + organization + "/ds/" + dataset + "/meta/dump";

        auto res = cpr::Post(cpr::Url(metaDumpUrl),
                             utils::authCookie(this->authToken), cpr::Payload{{"ids", json(ids).dump()}},
                             cpr::VerifySsl(this->sslVerify));

        if (res.status_code != 200)
            this->handleError(res);

        json metaDump = res.text;

        // Quick sanity check
        if (metaDump.is_array())
        {
            return metaDump;
        }
        else
        {
            throw RegistryException("Invalid meta dump: " + metaDump.dump());
        }
    }

    void ensureParentFolderExists(const fs::path &folder)
    {
        if (folder.has_parent_path())
        {
            const auto parentPath = folder.parent_path();
            io::assureFolderExists(parentPath);
        }
    }

    std::vector<Conflict> applyDelta(const Delta &d, const fs::path &sourcePath, Database *destination, const MergeStrategy mergeStrategy, const json &sourceMetaDump, std::ostream &out)
    {
        std::vector<Conflict> conflicts;

        // File operations
        if (d.adds.size() > 0 || d.removes.size() > 0)
        {
            fs::path destPath = destination->rootDirectory();
            const std::string tmpFolderName = (fs::path(DDB_FOLDER) / "tmp" / utils::generateRandomString(8)).string();

            const auto tempPath = destPath / tmpFolderName;

            if (fs::exists(tempPath))
            {
                io::assureIsRemoved(tempPath);
            }
            io::assureFolderExists(tempPath);

            Entry e;

            json debug = d;
            // LOGD << debug.dump(4);

            if (d.removes.empty())
            {
                // LOGD << "No removes in delta";
            }
            else
            {
                // LOGD << "Working on removes";

                for (const auto &rem : d.removes)
                {
                    // LOGD << rem.toString();

                    const auto dest = destPath / rem.path;

                    // LOGD << "Dest = " << dest;

                    // Check if database has modified the entry to be deleted
                    // if so, warn user and exit, unless a merge strategy
                    // has been specified.

                    // Currently we don't check if the file has been
                    // modified on the FS, perhaps we should?
                    bool indexed = true;

                    if (getEntry(destination, rem.path, e))
                    {
                        if (rem.hash != e.hash)
                        {
                            if (mergeStrategy == MergeStrategy::DontMerge)
                            {
                                conflicts.push_back(Conflict(rem.path, ConflictType::RemoteDeleteLocalModified));
                                continue; // Skip
                            }
                            else if (mergeStrategy == MergeStrategy::KeepOurs)
                            {
                                continue; // Skip
                            }
                            else if (mergeStrategy == MergeStrategy::KeepTheirs)
                            {
                                // Continue as normal
                            }
                        }
                    }
                    else
                    {
                        indexed = false;
                    }

                    if (fs::exists(dest))
                    {
                        if (indexed)
                            removeFromIndex(destination, {dest.string()});
                        io::assureIsRemoved(dest);
                        out << "D\t" << rem.path << std::endl;
                    }
                }
            }

            if (d.adds.empty())
            {
                // LOGD << "No adds in delta";
            }
            else
            {
                // LOGD << "Working on adds";

                for (const auto &add : d.adds)
                {
                    // LOGD << add.toString();

                    const auto source = sourcePath / add.path;
                    const auto dest = destPath / add.path;

                    // Check if the database has a modified entry
                    // for the same paths we are adding
                    if (getEntry(destination, add.path, e))
                    {
                        if (add.hash != e.hash)
                        {
                            if (mergeStrategy == MergeStrategy::DontMerge)
                            {
                                conflicts.push_back(Conflict(add.path, ConflictType::BothModified));
                                continue; // Skip
                            }
                            else if (mergeStrategy == MergeStrategy::KeepOurs)
                            {
                                continue; // Skip
                            }
                            else if (mergeStrategy == MergeStrategy::KeepTheirs)
                            {
                                // Continue as normal
                            }
                        }
                    }

                    if (add.isDirectory())
                    {
                        io::createDirectories(dest);
                    }
                    else
                    {
                        io::copy(source, dest);
                    }

                    // TODO: this could be made faster for large files
                    // by passing the already known hash instead
                    // of computing it
                    addToIndex(destination, {dest.string()},
                               [&out](const Entry &e, bool updated)
                               {
                                   out << (updated ? "U" : "A") << "\t" << e.path << std::endl;
                                   return true;
                               });
                }
            }

            if (fs::exists(tempPath))
            {
                io::assureIsRemoved(tempPath);
            }

            if (conflicts.size() == 0)
            {
                auto mPathList = d.modifiedPathList();
                if (mPathList.size() > 0)
                    syncLocalMTimes(destination, mPathList);
            }
        }

        // Early exit in case of conflicts
        if (conflicts.size() > 0)
            return conflicts;

        // Meta operations
        if (d.metaAdds.size() > 0)
        {
            json metaRestore = json::array();

            std::unordered_map<std::string, bool> metaIds;
            for (auto &id : d.metaAdds)
                metaIds[id] = true;

            for (auto &meta : sourceMetaDump)
            {
                if (!meta.contains("id"))
                    throw InvalidArgsException("Invalid meta element: " + meta.dump());
                if (metaIds.find(meta["id"]) != metaIds.end())
                    metaRestore.push_back(meta);
            }

            destination->getMetaManager()->restore(metaRestore);
        }

        if (d.metaRemoves.size() > 0)
        {
            destination->getMetaManager()->bulkRemove(d.metaRemoves);
        }

        return conflicts;
    }

    void Registry::pull(const std::string &path, const MergeStrategy mergeStrategy,
                        std::ostream &out)
    {
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

        const auto tempDdbFolder = db->ddbDirectory() / "tmp" / "pull_cache" /
                                   (tagInfo.organization + "-" + tagInfo.dataset);

        if (fs::exists(tempDdbFolder))
        {
            // TODO: there might be ways to resume downloads if a user CTRL+Cs
            io::assureIsRemoved(tempDdbFolder);
        }

        // Get stamp from registry
        json remoteStamp = this->getStamp(tagInfo.organization, tagInfo.dataset);

        // Perform local diff using delta method using last stamp
        SyncManager sm(db.get());
        const auto delta = getDelta(remoteStamp, sm.getLastStamp(tagInfo.registryUrl));

        if (!delta.empty())
        {
            out << "Delta: files (+" << delta.adds.size() << ",-" << delta.removes.size() << "), meta (+"
                << delta.metaAdds.size() << ",-" << delta.metaRemoves.size() << ")"
                << std::endl;
        }

        json remoteMetaDump = json::array();

        if (delta.metaAdds.size() > 0)
        {
            // Get meta dump
            remoteMetaDump = this->getMetaDump(tagInfo.organization, tagInfo.dataset, delta.metaAdds);
        }

        const auto tempNewFolder = tempDdbFolder / std::to_string(time(nullptr));

        // Let's download only if we have anything to download
        if (!delta.adds.empty())
        {
            // LOGD << "Temp new folder = " << tempNewFolder;

            auto localMap = computeDeltaLocals(delta, db.get(), tempNewFolder.string());

            const auto filesToDownload =
                boolinq::from(delta.adds)
                    .where(
                        [&localMap](const AddAction &add)
                        { return !add.isDirectory() && localMap.find(add.hash) == localMap.end(); })
                    .select([](const AddAction &add)
                            { return add.path; })
                    .toStdVector();

            // Download all the missing files
            this->downloadFiles(tagInfo.organization, tagInfo.dataset,
                                filesToDownload, tempNewFolder.string(), out);

            // LOGD << "Files downloaded";
        }
        else
        {
            // LOGD << "No files to download";
        }

        // TODO: speedup: we should be able to check for conflicts
        // before we download the files

        // Apply changes to local files
        auto conflicts = applyDelta(delta, tempNewFolder, db.get(), mergeStrategy, remoteMetaDump, out);
        io::assureIsRemoved(tempNewFolder);

        if (conflicts.size() == 0)
        {
            // No errors? Update stamp
            sm.setLastStamp(tagInfo.registryUrl, remoteStamp);
        }
        else
        {
            out << "Found conflicts, but don't worry! Make a copy of the conflicting entries and use --keep-theirs or --keep-ours to finish the pull:" << std::endl
                << std::endl;

            for (auto &c : conflicts)
            {
                out << "C\t" << c.path << " (" << c.description() << ")" << std::endl;
            }
        }

        db->close();

        // Cleanup
        io::assureIsRemoved(tempDdbFolder);
        io::assureIsRemoved(tempNewFolder);

        // Inform user if nothing was needed
        if (delta.empty())
        {
            out << "Everything up-to-date" << std::endl;
        }
        else
        {
            out << "Pull completed" << std::endl;
        }
    }

    std::unordered_map<std::string, bool> computeDeltaLocals(Delta d, Database *db, const std::string &hlDestFolder)
    {
        // Do we have files locally? If so we don't need to
        // download them, we just create hard links (or copies) to it after
        // validating that they are indeed the same
        // This function creates hard links only if hlDestFolder is set
        // Otherwise it just returns the map of valid local hashes
        const auto q = db->query("SELECT path,mtime FROM entries WHERE hash = ?");
        std::unordered_map<std::string, bool> localMap;

        std::unordered_map<std::string, bool> addsMap;
        for (auto &add : d.adds)
        {
            if (add.hash.empty())
                continue;
            addsMap[add.path] = true;
        }

        for (auto &add : d.adds)
        {
            if (add.hash.empty())
                continue;

            q->bind(1, add.hash);
            if (q->fetch())
            {
                auto ePath = q->getText(0);
                // Check the filesystem to make sure this hasn't been modified
                io::Path p(db->rootDirectory() / ePath);
                long long eMtime = q->getInt64(1);
                bool valid = false;
                if (p.getModifiedTime() == eMtime)
                {
                    valid = true;
                }
                else
                {
                    // Actually compute hash
                    valid = Hash::fileSHA256(p.get().string()) == add.hash;
                }

                if (valid)
                {
                    if (!hlDestFolder.empty())
                    {
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

    void Registry::push(const std::string &path, std::ostream &out)
    {
        auto db = open(path, true);

        TagManager tagManager(db.get());
        SyncManager syncManager(db.get());

        // Get our tag using tagmanager
        const auto tag = tagManager.getTag();

        if (tag.empty())
            throw IndexException("Cannot push if no tag is specified");

        // LOGD << "Tag = " << tag;

        const auto tagInfo = RegistryUtils::parseTag(tag);

        try
        {
            // 3) Get dataset info
            const auto dsInfo = this->getDatasetInfo(tagInfo.organization, tagInfo.dataset);

            out << "Pushing to '" << tag << "'" << std::endl;
        }
        catch (RegistryNotFoundException &ex)
        {
            // LOGD << "Dataset not found: " << ex.what();

            out << "Pushing to new '" << tag << "'" << std::endl;
        }

        // Initialize server push
        // LOGD << "Initializing server push";

        // Call POST endpoint passing database stamp
        PushManager pushManager(this, tagInfo.organization, tagInfo.dataset);

        std::string registryStampChecksum = "";
        try
        {
            const auto regStamp = syncManager.getLastStamp(tagInfo.registryUrl);
            registryStampChecksum = regStamp["checksum"];
        }
        catch (const NoStampException &)
        {
            // Nothing, this is the first time we push
        }

        const auto pir = pushManager.init(registryStampChecksum, db->getStamp());

        // LOGD << "Push initialized";

        // Push meta
        if (pir.neededMeta.size() > 0)
        {
            out << "Transferring metadata (" << pir.neededMeta.size() << ")" << std::endl;
            pushManager.meta(db->getMetaManager()->dump(pir.neededMeta), pir.token);
        }

        const auto basePath = db->rootDirectory();

        for (const auto &file : pir.neededFiles)
        {
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

    void Registry::handleError(cpr::Response &res)
    {
        if (res.status_code == 401)
            throw AuthException("Unauthorized");

        if (res.status_code != 200)
        {
            // LOGD << "Request error: " << res.text;

            json j = res.text;
            if (j.contains("error"))
                throw RegistryException("Error response from registry: " +
                                        j["error"].get<std::string>());
            throw RegistryException("Invalid response from registry: " +
                                    res.text);
        }

        // LOGD << "Request error: " << res.error.code << " -> " << res.error.message;

        throw RegistryException(
            "Invalid response from registry. Returned status: " +
            std::to_string(res.status_code));
    }

} // namespace ddb
