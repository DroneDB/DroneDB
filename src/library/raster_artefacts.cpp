/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_artefacts.h"

#include <cpl_conv.h>
#include <cpl_http.h>

#include <algorithm>
#include <iomanip>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

#include "build_layout.h"
#include "database.h"
#include "entry_handler.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "hash.h"
#include "logger.h"
#include "randomutil.h"
#include "timeutil.h"
#include "userprofile.h"

namespace ddb
{
    namespace raster
    {
        namespace
        {
            // The roles of the builds whose outputs libddb itself writes and GDAL reads again
            // (the COG of a raster, the GeoPackage and the tiles of a vector)
            constexpr const char *kReadableRoles[] = {"raster.cog", "vector.gpkg", "vector.mvt"};

            // The drivers libddb writes its artefacts with. A VRT is not among them: it lists
            // other files to read, and an artefact is one file.
            const std::vector<std::string> kRasterDrivers = {"GTiff", "COG"};
            const std::vector<std::string> kVectorDrivers = {"GPKG"};

            // The scratch folders of the helper requests are named with this
            constexpr const char *kRequestFolderMark = "req-";

            // Whether `path` is below `folder`, both resolved (`path` is not `folder` itself)
            bool isInside(const fs::path &path, const fs::path &folder)
            {
                const fs::path relative = path.lexically_relative(folder);
                if (relative.empty() || relative == ".")
                    return false;
                return *relative.begin() != "..";
            }

            // The path under "<hash>" of the build root that holds `path`, '/'-separated; nullopt
            // when `path` is not in "<something>/.ddb/build/<hash>/"
            std::optional<std::string> buildRelativePath(const fs::path &path)
            {
                const std::vector<fs::path> parts(path.begin(), path.end());
                for (size_t i = 0; i + 3 < parts.size(); i++)
                {
                    if (parts[i] != DDB_FOLDER || parts[i + 1] != DDB_BUILD_PATH ||
                        !isHashLike(io::toUtf8(parts[i + 2])))
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

            // While an artefact opens GDAL makes no HTTP request and its network file systems
            // (/vsicurl/ ...) open no other file than the one being opened, as when a file of a
            // user opens (gdal_open_policy.cpp): an artefact is a file libddb wrote, but what the
            // helper copied into it came from a file of a user
            class NoNetworkWhileOpening
            {
              public:
                explicit NoNetworkWhileOpening(const std::string &path)
                    : pushed_(CPLHTTPPushFetchCallback(&refuse, nullptr) != FALSE),
                      onlyThisFile_("CPL_VSIL_CURL_ALLOWED_FILENAME", path.c_str())
                {
                }
                ~NoNetworkWhileOpening()
                {
                    if (pushed_)
                        CPLHTTPPopFetchCallback();
                }
                NoNetworkWhileOpening(const NoNetworkWhileOpening &) = delete;
                NoNetworkWhileOpening &operator=(const NoNetworkWhileOpening &) = delete;

              private:
                static CPLHTTPResult *refuse(const char *url, CSLConstList, GDALProgressFunc, void *,
                                             CPLHTTPFetchWriteFunc, void *, void *)
                {
                    LOGW << "Refused to request " << url << " while opening an artefact";
                    auto *result = static_cast<CPLHTTPResult *>(CPLCalloc(1, sizeof(CPLHTTPResult)));
                    result->nStatus = 1;
                    result->pszErrBuf = CPLStrdup("no request is made while an artefact opens");
                    return result;
                }

                bool pushed_;
                ScopedThreadConfigOption onlyThisFile_;
            };

            GDALDatasetUniquePtr openArtefact(const fs::path &path, const std::vector<std::string> &drivers,
                                              unsigned int kind)
            {
                if (!isLibddbArtefact(path))
                    throw GDALException("Refusing to open " + io::toUtf8(path) +
                                        ": it is not a file libddb wrote (convert it with the raster helper first)");
                const std::string utf8 = io::toUtf8(path);
                const CPLStringList allowed = cslFromStrings(drivers);
                const NoNetworkWhileOpening noNetwork(utf8);
                return adoptDataset(GDALOpenEx(utf8.c_str(), kind | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR,
                                               allowed.List(), nullptr, nullptr));
            }
        } // namespace

        fs::path artefactsDir()
        {
            return UserProfile::get()->getCachePath("artefacts");
        }

        bool isLibddbArtefact(const fs::path &path)
        {
            if (path.empty())
                return false;
            std::error_code ec;
            const fs::path resolved = fs::weakly_canonical(fs::absolute(path, ec), ec);
            if (ec)
                return false;

            if (const auto relative = buildRelativePath(resolved); relative && isReadableBuildOutput(*relative))
                return true;

            const fs::path folder = fs::weakly_canonical(artefactsDir(), ec);
            return !ec && isInside(resolved, folder);
        }

        std::string artefactKey(const fs::path &source)
        {
            // The modification time with the fraction of the second the file system keeps: a raster
            // replaced in the same second by one of the same size is another file
            io::Path file(source);
            const io::FileState state = file.getState();
            std::ostringstream key;
            key << io::toUtf8(source) << "*" << std::fixed << std::setprecision(6) << state.mtime << "*" << state.size;
            return Hash::strCRC64(key.str());
        }

        GDALDatasetUniquePtr openArtefactRaster(const fs::path &path)
        {
            return openArtefact(path, kRasterDrivers, GDAL_OF_RASTER);
        }

        GDALDatasetUniquePtr openArtefactVector(const fs::path &path)
        {
            return openArtefact(path, kVectorDrivers, GDAL_OF_VECTOR);
        }

        io::ScopedTempDir makePrivateFolder(const fs::path &parent, const std::string &prefix)
        {
            io::ScopedTempDir folder = io::ScopedTempDir::create(parent, prefix);
            std::error_code ec;
            fs::permissions(folder.path(), fs::perms::owner_all, fs::perm_options::replace, ec);
            if (ec)
                throw FSException("Cannot restrict the access to " + io::toUtf8(folder.path()), ec);
            return folder;
        }

        io::ScopedTempDir makeArtefactRequestFolder(const std::string &prefix)
        {
            return makePrivateFolder(artefactsDir(), kRequestFolderMark + prefix);
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

        void cleanupArtefacts(time_t maxAge)
        {
            const time_t threshold = currentUnixTimestamp() - maxAge;
            std::error_code ec;
            for (fs::directory_iterator it(artefactsDir(), ec), end; !ec && it != end; it.increment(ec))
            {
                const fs::path entry = it->path();
                try
                {
                    if (io::Path(entry).getModifiedTime() < threshold)
                        io::assureIsRemoved(entry);
                }
                catch (const std::exception &e)
                {
                    // A file a reader still holds (Windows) stays until the next time
                    LOGD << "Cannot remove the artefact " << io::toUtf8(entry) << ": " << e.what();
                }
            }
        }

    } // namespace raster
} // namespace ddb
