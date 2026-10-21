/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "tilerhelper.h"
#include "gdaltiler.h"
#include "threadlock.h"
#include "raster_cache.h"
#include "randomutil.h"

#include <functional>

#include <memory>
#include <vector>

#include "cancellation.h"
#include "entry.h"
#include "entry_handler.h"
#include "exceptions.h"
#include "geoproject.h"
#include "hash.h"
#include "logger.h"
#include <cpr/cpr.h>
#include "mio.h"
#include "userprofile.h"

namespace ddb
{

    BoundingBox<int> TilerHelper::parseZRange(const std::string &zRange)
    {
        BoundingBox<int> r;

        const std::size_t dashPos = zRange.find('-');
        if (dashPos != std::string::npos)
        {
            r.min = std::stoi(zRange.substr(0, dashPos));
            r.max = std::stoi(zRange.substr(dashPos + 1, zRange.length() - 1));
            if (r.min > r.max)

                std::swap(r.min, r.max);

        }
        else
        {
            r.min = r.max = std::stoi(zRange);
        }

        return r;
    }

    fs::path TilerHelper::getCacheFolderName(const fs::path &tileablePath,
                                             time_t modifiedTime, int tileSize)
    {
        std::ostringstream os;
        os << io::toUtf8(tileablePath) << "*" << modifiedTime << "*" << tileSize;
        return Hash::strCRC64(os.str());
    }

    fs::path TilerHelper::getFromUserCache(const fs::path &tileablePath, int tz,
                                           int tx, int ty, int tileSize, bool tms,
                                           bool forceRecreate,
                                           const std::string &tileablePathHash)
    {
        if (!fs::exists(tileablePath))
            throw FSException(io::toUtf8(tileablePath) + " does not exist");
        if (oneIn(1000))
            cleanupUserCache();

        const time_t modifiedTime = io::Path(tileablePath).getModifiedTime();
        const fs::path tileCacheFolder =
            UserProfile::get()->getTilesDir() /
            getCacheFolderName(tileablePath, modifiedTime, tileSize);
        fs::path outputFile = tileCacheFolder / std::to_string(tz) /
                              std::to_string(tx) / (std::to_string(ty) + ".png");

        // Cache hit
        if (fs::exists(outputFile) && !forceRecreate)
        {
            return outputFile;
        }

        return TilerHelper::getTile(tileablePath, tz, tx, ty, tileSize, tms, forceRecreate, tileCacheFolder, nullptr, nullptr, tileablePathHash);
    }

    std::string TilerHelper::withTiler(const fs::path &tileablePath, int tileSize, bool tms, bool forceRecreate,
                                       const fs::path &outputFolder, const std::string &tileablePathHash,
                                       const std::function<std::string(Tiler &)> &render)
    {
        // Files an entry handler tiles itself (COPC point clouds) are read in place; the
        // others are downloaded when remote and tiled by GDAL. Each step checks whether the
        // call was canceled (DQ-15); the GDAL steps also while they run.
        cancel::throwIfCanceled();
        const TilerFactory *factory = findTilerFactory(tileablePath);
        const fs::path localPath = factory != nullptr ? tileablePath : toLocalFile(tileablePath, tileablePathHash);
        cancel::throwIfCanceled();

        // A forced recreation must not be served by a tiler prepared before it
        if (forceRecreate)
            RasterCache::instance().release(localPath);

        const TilerKey key{io::toUtf8(localPath), tileSize, tms, io::toUtf8(outputFolder)};
        auto tiler = RasterCache::instance().acquireTiler(key, [&]() -> std::unique_ptr<Tiler> {
            if (factory != nullptr)
                return factory->create(localPath, outputFolder, tileSize, tms);

            const fs::path fileToTile = toGeoTIFF(localPath, tileSize, forceRecreate, "", tileablePathHash);
            return std::make_unique<GDALTiler>(io::toUtf8(fileToTile), io::toUtf8(outputFolder), tileSize, tms);
        });
        cancel::throwIfCanceled();
        return render(*tiler);
    }

    fs::path TilerHelper::getTile(const fs::path &tileablePath, int tz, int tx, int ty, int tileSize, bool tms, bool forceRecreate, const fs::path &outputFolder, uint8_t **outBuffer, int *outBufferSize, const std::string &tileablePathHash)
    {
        return getTile(tileablePath, tz, tx, ty, tileSize, tms, forceRecreate, outputFolder, std::string("png"),
                       outBuffer, outBufferSize, tileablePathHash);
    }

    fs::path TilerHelper::getTile(const fs::path &tileablePath, int tz, int tx, int ty, int tileSize, bool tms, bool forceRecreate, const fs::path &outputFolder, const std::string &outputFormat, uint8_t **outBuffer, int *outBufferSize, const std::string &tileablePathHash)
    {
        const ImageFormat format = parseImageFormat(outputFormat);
        return io::fromUtf8(withTiler(tileablePath, tileSize, tms, forceRecreate, outputFolder, tileablePathHash,
                                      [&](Tiler &t) { return t.tile(tz, tx, ty, format, outBuffer, outBufferSize); }));
    }

    fs::path TilerHelper::getTile(const fs::path &tileablePath, int tz, int tx, int ty, int tileSize, bool tms, bool forceRecreate, const fs::path &outputFolder, const ThumbVisParams &visParams, uint8_t **outBuffer, int *outBufferSize, const std::string &tileablePathHash)
    {
        return io::fromUtf8(withTiler(tileablePath, tileSize, tms, forceRecreate, outputFolder, tileablePathHash,
                                      [&](Tiler &t) {
                                          // Visualization parameters apply to rasters; point clouds render as usual
                                          if (auto *raster = dynamic_cast<GDALTiler *>(&t))
                                              return raster->tile(tz, tx, ty, visParams, outBuffer, outBufferSize);
                                          return t.tile(tz, tx, ty, outBuffer, outBufferSize);
                                      }));
    }

    fs::path TilerHelper::toLocalFile(const fs::path &tileablePath, const std::string &tileablePathHash)
    {
        fs::path localTileablePath;

        if (utils::isNetworkPath(io::toUtf8(tileablePath)))
        {
            // Download file to user cache
            const std::string ext = io::toUtf8(fs::path(tileablePath).extension());

            // If we know a priori the hash of the remote resource,
            // we use that value to search our local cache (to avoid
            // downloading things twice)
            bool alwaysDownload = false;
            if (!tileablePathHash.empty())
            {
                localTileablePath = UserProfile::get()->getTilesDir() /
                                    io::fromUtf8(tileablePathHash + ext);

                // Download only if not exists
                alwaysDownload = false;
            }
            else
            {
                std::string crc = Hash::strCRC64(io::toUtf8(tileablePath));
                localTileablePath = UserProfile::get()->getTilesDir() / io::fromUtf8(crc + ext);
                alwaysDownload = true; // always download, content could have changed
            }

            // One thread at a time
            {
                ThreadLock lock(io::toUtf8(localTileablePath));

                bool download = alwaysDownload || !fs::exists(localTileablePath);
                if (download)
                {
                    // Download to a temp file so a failed transfer never lands in the cache
                    const fs::path tmpPath = io::concat(localTileablePath, ".download-" + utils::generateRandomString(16));
                    cpr::Response res;
                    bool writeOk = false;
                    {
                        std::ofstream of(tmpPath, std::ios::binary);
                        res = cpr::Download(of, cpr::Url(io::toUtf8(tileablePath)));
                        // cpr's write callback ignores stream errors, so a disk-full
                        // or I/O failure only shows up on the stream state
                        of.close();
                        writeOk = !of.fail();
                    }

                    if (!writeOk || res.error || res.status_code < 200 || res.status_code >= 300)
                    {
                        io::assureIsRemoved(tmpPath);
                        throw NetException("Cannot download " + io::toUtf8(tileablePath) + " (HTTP " +
                                           std::to_string(res.status_code) + "): " +
                                           (writeOk ? res.error.message : std::string("write error")));
                    }

                    // Tilers kept open on the previous download would block the replace on Windows
                    RasterCache::instance().release(localTileablePath);
                    io::rename(tmpPath, localTileablePath);
                }
            }
        }
        else
        {
            localTileablePath = tileablePath;
        }

        return localTileablePath;
    }

    fs::path TilerHelper::toGeoTIFF(const fs::path &tileablePath, int tileSize,
                                    bool forceRecreate,
                                    const fs::path &outputGeotiff,
                                    const std::string &tileablePathHash)
    {
        const fs::path localTileablePath = toLocalFile(tileablePath, tileablePathHash);

        if (entryHandler(fingerprint(localTileablePath)).tilesWithoutReprojection())
        {
            // Georasters can be tiled directly
            return localTileablePath;
        }
        else
        {
            fs::path outputPath = outputGeotiff;

            if (outputGeotiff.empty())
            {
                // Store in user cache if user doesn't specify a preference
                if (oneIn(1000))
                    cleanupUserCache();
                const time_t modifiedTime = io::Path(localTileablePath).getModifiedTime();
                const fs::path tileCacheFolder =
                    UserProfile::get()->getTilesDir() /
                    getCacheFolderName(localTileablePath, modifiedTime, tileSize);
                io::assureFolderExists(tileCacheFolder);

                outputPath = tileCacheFolder / "geoprojected.tif";
            }
            else
            {
                // Just make sure the parent path exists
                io::assureFolderExists(outputGeotiff.parent_path());
            }

            // We need to (attempt) to geoproject the file first
            if (!fs::exists(outputPath) || forceRecreate)
            {
                // Multiple threads could be generating the geoprojected
                // file at the same time, so we place a lock
                {
                    ThreadLock lock(io::toUtf8(outputPath));

                    // Another thread may have generated the file while we waited
                    // (geoProject publishes it via an atomic rename)
                    if (forceRecreate || !fs::exists(outputPath))
                    {
                        ddb::geoProject({io::toUtf8(localTileablePath)}, io::toUtf8(outputPath),
                                        "100%", true);
                    }
                }
            }

            return outputPath;
        }
    }

    void TilerHelper::cleanupUserCache()
    {
        LOGD << "Cleaning up tiles user cache";

        const time_t threshold =
            utils::currentUnixTimestamp() - 60 * 60 * 24 * 5; // 5 days
        const fs::path tilesDir = UserProfile::get()->getTilesDir();

        // Iterate directories
        for (auto d = fs::recursive_directory_iterator(tilesDir);
             d != fs::recursive_directory_iterator(); ++d)
        {
            fs::path dir = d->path();
            if (fs::is_directory(dir))
            {
                if (io::Path(dir).getModifiedTime() < threshold)
                {
                    io::assureIsRemoved(dir);
                }
            }
        }
    }

    void TilerHelper::runTiler(const fs::path &input,
                               const fs::path &output,
                               int tileSize, bool tms,
                               std::ostream &os,
                               const std::string &format, const std::string &zRange,
                               const std::string &x, const std::string &y)
    {
        // Owned: parseZRange, std::stoi and tile() throw on bad input or failed tiles
        std::unique_ptr<Tiler> tiler;

        if (const TilerFactory *factory = findTilerFactory(input))
        {
            // Tiled by its entry handler (COPC point cloud)
            tiler = factory->create(input, output, tileSize, tms);
        }
        else
        {
            // Assume image/geotiff
            fs::path geotiff = ddb::TilerHelper::toGeoTIFF(input, tileSize, true);
            tiler = std::make_unique<GDALTiler>(io::toUtf8(geotiff), io::toUtf8(output), tileSize, tms);
        }

        BoundingBox<int> zb;
        if (zRange == "auto")
        {
            zb = tiler->getMinMaxZ();
        }
        else
        {
            zb = parseZRange(zRange);
        }

        const bool json = format == "json";

        if (json)
        {
            os << "[";
        }

        for (int z = zb.min; z <= zb.max; z++)
        {
            if (x != "auto" && y != "auto")
            {
                // Just one tile
                if (json)
                    os << "\"";
                os << tiler->tile(z, std::stoi(x), std::stoi(y));
                if (json)
                    os << "\"";
                else
                    os << std::endl;
            }
            else
            {
                // All tiles
                std::vector<ddb::TileInfo> tiles = tiler->getTilesForZoomLevel(z);
                for (auto &t : tiles)
                {
                    if (json)
                        os << "\"";

                    LOGD << "Tiling " << t.tx << " " << t.ty << " " << t.tz;
                    os << tiler->tile(t);

                    if (json)
                    {
                        os << "\"";
                        if (&t != &tiles[tiles.size() - 1])
                            os << ",";
                    }
                    else
                    {
                        os << std::endl;
                    }
                }
            }
        }

        if (json)
        {
            os << "]";
        }
    }

} // namespace ddb
