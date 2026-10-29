/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "artefact_cache.h"

#include <chrono>
#include <system_error>

#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "raster_artefacts.h"
#include "raster_helper.h"

namespace ddb
{
    namespace raster
    {
        namespace
        {
            // The name of the copy in the folder of its request, before it is moved to its own
            constexpr const char *kProducedName = "normalized.tif";

            // Writes the copy of `path` to `target` through the raster helper
            void produceNormalized(const fs::path &path, const fs::path &target)
            {
                const io::ScopedTempDir request = makeArtefactRequestFolder("normalize-");
                const fs::path produced = request.path() / kProducedName;
                rasterHelper().normalize(path, produced, json::object());

                // What the helper wrote counts only when it opens as a GeoTIFF: it is a file that
                // came out of a process that read a file of a user
                const auto opened = openArtefactRaster(produced);
                if (!opened)
                    throw GDALException(std::string(kGdalUtilName) + " wrote a normalized copy of " +
                                        io::toUtf8(path) + " that is not a GeoTIFF");

                // A caller that asked at the same time may have published the same copy: it is
                // the same file, and a reader may hold it
                if (!fs::exists(target))
                    publishArtefact(produced, target);
            }
        } // namespace

        fs::path artefactFor(const fs::path &path)
        {
            if (isLibddbArtefact(path))
                return path;

            const fs::path target = normalizedDir() / io::fromUtf8(artefactKey(path) + ".tif");
            std::error_code ec;
            if (fs::exists(target, ec))
            {
                // Used: not one of the copies the cleanup removes
                fs::last_write_time(target, fs::file_time_type::clock::now(), ec);
                return target;
            }

            LOGD << "Normalizing " << io::toUtf8(path);
            produceNormalized(path, target);
            return target;
        }

        GDALDatasetUniquePtr openInputRaster(const std::string &path)
        {
            const fs::path file = io::fromUtf8(path);
            // A raster that is not there has no copy to make. An artefact is opened as it is, and
            // GDAL says in the log why it cannot (the COG of a build that is gone)
            std::error_code ec;
            if (!isLibddbArtefact(file) && !fs::exists(file, ec))
                return nullptr;

            try
            {
                return openArtefactRaster(artefactFor(file));
            }
            catch (const RasterOpException &e)
            {
                // The raster cannot be opened, or the rules refuse it: a raster that does not
                // open, as for any file GDAL cannot read. A failure of the helper itself is not.
                if (e.code() == ErrorCode::Internal)
                    throw;
                LOGD << "Cannot open " << path << ": " << e.what();
                return nullptr;
            }
        }
    } // namespace raster
} // namespace ddb
