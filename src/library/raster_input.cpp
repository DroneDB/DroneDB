/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_input.h"

#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>

#include "exceptions.h"
#include "fileversion.h"
#include "gdal_open_policy.h"
#include "logger.h"
#include "mio.h"
#include "parallel.h"
#include "raster_helper.h"
#include "raster_protocol.h"
#include "raster_stats.h"
#include "threadlock.h"

namespace ddb
{
    namespace raster
    {
        namespace
        {
            // The name of the copy in the folder of its request
            constexpr const char *kProducedName = "normalized.tif";

            std::atomic<uint64_t> copiesMade{0};

            // The answers of isHostReadable, by resolved path and version of the file
            std::mutex hostReadableMutex;
            std::map<std::string, bool> hostReadableAnswers;

            std::optional<bool> knownAnswer(const std::string &key)
            {
                const std::lock_guard lock(hostReadableMutex);
                const auto it = hostReadableAnswers.find(key);
                return it == hostReadableAnswers.end() ? std::nullopt : std::optional<bool>(it->second);
            }

            // Whether `path` starts as a TIFF or a BigTIFF does ("II" or "MM", then 42 or 43 in that byte
            // order): ddb-tilers opens nothing else, so another file (a photo, a JPEG 2000) is read through
            // a copy without asking the helper first. Four bytes, read as bytes.
            bool startsAsTiff(const fs::path &path)
            {
                std::ifstream in(path, std::ios::binary);
                unsigned char head[4] = {};
                if (!in.read(reinterpret_cast<char *>(head), sizeof(head)))
                    return false;
                const bool little = head[0] == 'I' && head[1] == 'I' && head[3] == 0 && (head[2] == 42 || head[2] == 43);
                const bool big = head[0] == 'M' && head[1] == 'M' && head[2] == 0 && (head[3] == 42 || head[3] == 43);
                return little || big;
            }

            // What the helper's validation says of `path`; the statistics it counted are adopted.
            // A failure of the helper is an answer (not host readable) unless the call was canceled or
            // the helper cannot run at all.
            bool validateInHelper(const fs::path &path)
            {
                const size_t wanted = static_cast<size_t>(parallel::threadsOption("DDB_STATS_THREADS", 8));
                const auto cpu = parallel::tryAcquireThreads(wanted);
                ValidateParams params;
                params.threads = static_cast<int>(cpu.threads());
                json result;
                try
                {
                    result = rasterHelper().operate(op::kValidate, fs::absolute(path), fs::path(), toJson(params), {});
                }
                catch (const CanceledException &)
                {
                    throw;
                }
                catch (const HelperUnavailableException &)
                {
                    throw;
                }
                catch (const AppException &e)
                {
                    LOGW << "The validation of " << io::toUtf8(path) << " failed, it is read through a copy: "
                         << e.what();
                    return false;
                }

                try
                {
                    if (!result.at("hostReadable").get<bool>())
                    {
                        LOGD << io::toUtf8(path) << " is read through a copy: "
                             << result.value("reason", std::string("no reason given"));
                        return false;
                    }
                    RasterStatsProvider::instance().adopt(io::toUtf8(path), statisticsByBandFromJson(result.at("bands")));
                    return true;
                }
                catch (const std::exception &e)
                {
                    LOGW << kGdalUtilName << " gave a validation of " << io::toUtf8(path)
                         << " that cannot be used, it is read through a copy: " << e.what();
                    return false;
                }
            }

            // A copy of `path` that the helper writes into a new request folder
            InputFile transientCopy(const fs::path &path)
            {
                auto request = std::make_unique<RequestFolder>("normalize-");
                const fs::path produced = request->path() / kProducedName;
                rasterHelper().normalize(path, produced, json::object());

                // What the helper wrote counts only when it reads as a GeoTIFF: it is a file that
                // came out of a process that read a file of a user
                if (!writtenRasterOpens(produced))
                    throw GDALException(std::string(kGdalUtilName) + " wrote a normalized copy of " +
                                        io::toUtf8(path) + " that is not a GeoTIFF");
                copiesMade++;
                return InputFile{produced, std::move(request)};
            }

            void requireHost(const char *what)
            {
                // The helper must not ask another helper, and its workers can neither start a program
                // nor write outside the folder of their request
                if (gdalopen::userFileOpensEnabled())
                    throw std::logic_error(std::string(what) +
                                           " runs in the host: the raster helper reads the file of its request "
                                           "itself (openInputRaster)");
            }
        } // namespace

        bool isHostReadable(const fs::path &path)
        {
            requireHost("raster::isHostReadable");
            const std::string version = fileVersion(path);
            if (version.empty())
                throw FSException("Cannot read " + io::toUtf8(path));
            std::error_code ec;
            const fs::path resolved = fs::weakly_canonical(fs::absolute(path, ec), ec);
            if (ec)
                throw FSException("Cannot resolve " + io::toUtf8(path), ec);
            const std::string key = cacheKeyPath(resolved) + "*" + version;
            if (const auto known = knownAnswer(key))
                return *known;
            if (!startsAsTiff(path))
                return false;

            // One validation of a file at a time: a reader that asked meanwhile takes its answer
            ThreadLock lock("host-readable:" + key);
            if (const auto known = knownAnswer(key))
                return *known;
            LOGD << "Validating " << io::toUtf8(path);
            const bool readable = validateInHelper(path);
            const std::lock_guard guard(hostReadableMutex);
            hostReadableAnswers[key] = readable;
            return readable;
        }

        void forgetHostReadable()
        {
            const std::lock_guard lock(hostReadableMutex);
            hostReadableAnswers.clear();
        }

        InputFile inputFileFor(const fs::path &path)
        {
            requireHost("raster::inputFileFor");
            if (isLibddbArtefact(path) || isHostReadable(path))
                return InputFile{path, nullptr};
            LOGD << "Normalizing " << io::toUtf8(path);
            return transientCopy(path);
        }

        uint64_t inputCopiesMade()
        {
            return copiesMade.load();
        }

        GDALDatasetUniquePtr openInputRaster(const std::string &path)
        {
            // The host reads rasters with ddb-tilers (tilers::Raster::openInput), never with GDAL
            if (!gdalopen::userFileOpensEnabled())
                throw std::logic_error("raster::openInputRaster runs in the host: the raster helper reads rasters "
                                       "with ddb-tilers");
            return openUserRaster(path);
        }
    } // namespace raster
} // namespace ddb
