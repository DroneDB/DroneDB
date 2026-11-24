/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_artefacts.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>
#include <vector>

#include "build_layout.h"
#include "database.h"
#include "entry_handler.h"
#include "exceptions.h"
#include "fileversion.h"
#include "gdal_inc.h"
#include "gdal_open_policy.h"
#include "logger.h"
#include "process_config.h"
#include "userprofile.h"
#include "tilers_raster.h"
#include "randomutil.h"

#ifdef _WIN32
#include "sandbox/sandbox.h"
#endif

namespace ddb
{
    namespace raster
    {
        namespace
        {
            // The roles of the builds whose outputs libddb itself writes and GDAL reads again
            // (the COG of a raster, the GeoPackage and the tiles of a vector)
            constexpr const char *kReadableRoles[] = {"raster.cog", "vector.gpkg", "vector.mvt"};

            // The driver libddb writes its vector artefacts with
            const std::vector<std::string> kVectorDrivers = {"GPKG"};

            // A file of the memory file system of GDAL: what is in it was written by this process
            // (a segment ".." would leave the file system)
            bool isMemoryFile(const fs::path &path)
            {
                const std::string text = io::toUtf8(path);
                return text.rfind("/vsimem/", 0) == 0 && text.find("..") == std::string::npos;
            }

            // The path under "<key>" (build_layout.h) of the build root that holds `path`, '/'-separated;
            // nullopt when `path` is not in "<something>/.ddb/build/<key>/"
            std::optional<std::string> buildRelativePath(const fs::path &path)
            {
                const std::vector<fs::path> parts(path.begin(), path.end());
                for (size_t i = 0; i + 3 < parts.size(); i++)
                {
                    if (parts[i] != DDB_FOLDER || parts[i + 1] != DDB_BUILD_PATH ||
                        !isBuildKey(io::toUtf8(parts[i + 2])))
                        continue;
                    std::string relative;
                    for (size_t j = i + 3; j < parts.size(); j++)
                        relative += (relative.empty() ? "" : "/") + io::toUtf8(parts[j]);
                    return relative;
                }
                return std::nullopt;
            }

            // Whether `relative` (in a build folder) is the output of a role whose files GDAL
            // reads: the file of a file artefact, any file below the folder of a directory
            // artefact
            bool isReadableBuildOutput(const std::string &relative)
            {
                for (const EntryHandler *handler : entryHandlers())
                {
                    for (const BuildArtifact &artifact : handler->artifacts())
                    {
                        if (artifact.legacy)
                            continue;
                        const bool readable = std::any_of(std::begin(kReadableRoles), std::end(kReadableRoles),
                                                          [&](const char *role) { return artifact.role == role; });
                        if (!readable)
                            continue;
                        if (artifact.kind == ArtifactKind::File ? relative == artifact.path
                                                                : relative.rfind(artifact.path + "/", 0) == 0)
                            return true;
                    }
                }
                return false;
            }

            // Opens `path` read-only with `drivers` alone. While it opens GDAL makes
            // no HTTP request and opens no other file through its network file systems, as when
            // a file of a user opens: an artefact is a file libddb wrote, but what the helper
            // copied into it came from a file of a user. A dataset whose metadata names another
            // dataset is not opened either, since GDAL would open that one later with all its
            // drivers: the copy of a raster leaves none of those items, and one found here was
            // put there by something other than libddb.
            GDALDatasetUniquePtr openWithDrivers(const std::string &utf8, const std::vector<std::string> &drivers,
                                                 unsigned int kind)
            {
                const CPLStringList allowed = cslFromStrings(drivers);
                const gdalopen::NoNetworkWhileOpening noNetwork(utf8);
                auto dataset = adoptDataset(GDALOpenEx(
                    utf8.c_str(), kind | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR,
                    allowed.List(), nullptr, nullptr));
                if (!dataset)
                    return dataset;
                if (const auto verdict = gdalopen::validateMetadataFiles(datasetHandle(dataset)); !verdict.ok)
                {
                    LOGW << "Not opening " << utf8 << ": " << verdict.reason;
                    return nullptr;
                }
                return dataset;
            }

            GDALDatasetUniquePtr openArtefact(const fs::path &path, const std::vector<std::string> &drivers,
                                              unsigned int kind)
            {
                requireArtefact(path);
                return openWithDrivers(io::toUtf8(path), drivers, kind);
            }
        } // namespace

        namespace
        {
            // The folder of the cache that held the artefact cache of earlier versions
            constexpr const char *kLegacyArtefactsFolder = "artefacts";
            // The folder of the cache that holds the folders of requests
            constexpr const char *kRequestsFolder = "requests";

            // The request folders this process holds (RequestFolder), by resolved place
            std::mutex liveRequestsMutex;
            std::set<std::string> liveRequests;

            // Whether `resolved` (a cacheKeyPath) is inside the folder of a live request
            bool inLiveRequest(const std::string &resolved)
            {
                const std::lock_guard lock(liveRequestsMutex);
                return std::any_of(liveRequests.begin(), liveRequests.end(), [&](const std::string &folder) {
                    return resolved != folder && isCacheKeyUnder(resolved, folder);
                });
            }

            // The resolved place of `path` as the live requests are compared; empty when it cannot be resolved
            std::string resolvedKey(const fs::path &path)
            {
                std::error_code ec;
                const fs::path absolute = fs::absolute(path, ec);
                if (ec)
                    return {};
                const fs::path resolved = fs::weakly_canonical(absolute, ec);
                return ec ? std::string() : cacheKeyPath(resolved);
            }
        } // namespace

        fs::path requestsDir()
        {
            return ProcessConfig::get().cacheDir() / kRequestsFolder;
        }

        bool isLiveRequestFolder(const fs::path &folder)
        {
            const std::string key = resolvedKey(folder);
            const std::lock_guard lock(liveRequestsMutex);
            return !key.empty() && liveRequests.count(key) != 0;
        }

        RequestFolder::RequestFolder(const std::string &prefix)
        {
            std::error_code ec;
            fs::create_directories(requestsDir(), ec);
            if (ec)
                throw FSException("Cannot create " + io::toUtf8(requestsDir()), ec);
            folder_ = makePrivateFolder(requestsDir(), prefix);
            key_ = resolvedKey(folder_.path());
            if (key_.empty())
                throw FSException("Cannot resolve " + io::toUtf8(folder_.path()));
            const std::lock_guard lock(liveRequestsMutex);
            liveRequests.insert(key_);
        }

        RequestFolder::~RequestFolder()
        {
            // Not an artefact place any more before it is removed
            const std::lock_guard lock(liveRequestsMutex);
            liveRequests.erase(key_);
        }

        void requireArtefact(const fs::path &path)
        {
            if (!isLibddbArtefact(path))
                throw GDALException("Refusing to open " + io::toUtf8(path) +
                                    ": it is not a file libddb wrote (build the dataset, or copy it with the raster helper, first)");
        }

        bool isLibddbArtefact(const fs::path &path)
        {
            if (path.empty())
                return false;
            if (isMemoryFile(path))
                return true;
            std::error_code ec;
            const fs::path resolved = fs::weakly_canonical(fs::absolute(path, ec), ec);
            if (ec)
                return false;

            if (const auto relative = buildRelativePath(resolved); relative && isReadableBuildOutput(*relative))
                return true;

            return inLiveRequest(cacheKeyPath(resolved));
        }

        GDALDatasetUniquePtr openArtefactVector(const fs::path &path)
        {
            return openArtefact(path, kVectorDrivers, GDAL_OF_VECTOR);
        }

        bool writtenRasterOpens(const fs::path &path)
        {
            std::error_code ec;
            if (!fs::is_regular_file(fs::symlink_status(path, ec)) || ec)
            {
                LOGW << "Not reading " << io::toUtf8(path) << ": it is not a regular file";
                return false;
            }
            try
            {
                return tilers::Raster::openPrivate(path) != nullptr;
            }
            catch (const AppException &e)
            {
                LOGW << "Cannot read " << io::toUtf8(path) << ": " << e.what();
                return false;
            }
        }

        io::ScopedTempDir makePrivateFolder(const fs::path &parent, const std::string &prefix,
                                            PrivateFolderWriter writer)
        {
            io::ScopedTempDir folder = io::ScopedTempDir::create(parent, prefix);
            std::error_code ec;
            fs::permissions(folder.path(), fs::perms::owner_all, fs::perm_options::replace, ec);
            if (ec)
                throw FSException("Cannot restrict the access to " + io::toUtf8(folder.path()), ec);
#ifdef _WIN32
            // The gdal-util server runs at low integrity on Windows, and such a process writes
            // only below a folder with a low integrity label: the folder of every request gets
            // one (what the helper makes in it inherits it). Without the label the helper fails
            // to write and says so; with the sandbox off it needs none.
            // The folder of a writer that is not the helper gets none.
            std::string labelError;
            if (writer == PrivateFolderWriter::Helper &&
                !sandbox::windows::setLowIntegrityLabel(io::toUtf8(folder.path()), &labelError))
                LOGW << "Cannot give " << io::toUtf8(folder.path()) << " a low integrity label: " << labelError;
#else
            (void)writer;
#endif
            return folder;
        }

        void publishArtefact(const fs::path &produced, const fs::path &target)
        {
            std::error_code ec;
            fs::rename(produced, target, ec);
            if (!ec)
                return;
            if (ec != std::errc::cross_device_link)
                throw FSException("Cannot move " + io::toUtf8(produced) + " --> " + io::toUtf8(target) + " (" +
                                      ec.message() + ")",
                                  ec);

            // The target is on another device: the copy goes next to it, the rename finishes it
            const fs::path sibling = io::concat(target, ".copy-" + randomString(16));
            try
            {
                io::copy(produced, sibling);
                io::rename(sibling, target);
            }
            catch (...)
            {
                io::assureIsRemoved(sibling);
                throw;
            }
            io::assureIsRemoved(produced);
        }

        void removeLegacyArtefactCache()
        {
            // Looked at, not made: the cache folder may not exist yet
            const fs::path legacy = ProcessConfig::get().cacheDir() / kLegacyArtefactsFolder;
            std::error_code ec;
            if (!fs::exists(fs::symlink_status(legacy, ec)))
                return;
            const auto removed = fs::remove_all(legacy, ec);
            if (ec)
                LOGW << "Cannot remove the artefact cache of an earlier version, " << io::toUtf8(legacy) << ": "
                     << ec.message();
            else
                LOGI << "Removed the artefact cache of an earlier version, " << io::toUtf8(legacy) << " (" << removed
                     << " files)";
        }

    } // namespace raster
} // namespace ddb
