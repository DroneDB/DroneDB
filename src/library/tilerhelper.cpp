/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "tilerhelper.h"
#include "timeutil.h"
#include "gdaltiler.h"
#include "threadlock.h"
#include "raster_cache.h"
#include "randomutil.h"

#include <functional>

#include <memory>
#include <optional>
#include <vector>

#include "cancellation.h"
#include "entry.h"
#include "entry_handler.h"
#include "exceptions.h"
#include "geoproject.h"
#include "hash.h"
#include "json.h"
#include "logger.h"
#include "http_client.h"
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

    fs::path TilerHelper::getFromUserCache(const TileRequest &request)
    {
        const fs::path &tileablePath = request.source.path;
        if (!fs::exists(tileablePath))
            throw FSException(io::toUtf8(tileablePath) + " does not exist");
        if (oneIn(1000))
            cleanupUserCache();

        const time_t modifiedTime = io::Path(tileablePath).getModifiedTime();
        const fs::path tileCacheFolder =
            UserProfile::get()->getTilesDir() /
            getCacheFolderName(tileablePath, modifiedTime, request.source.tileSize);
        fs::path outputFile = tileCacheFolder / std::to_string(request.tz) /
                              std::to_string(request.tx) / (std::to_string(request.ty) + ".png");

        // Cache hit
        if (fs::exists(outputFile) && !request.source.forceRecreate)
        {
            return outputFile;
        }

        TileRequest cached;
        cached.source = request.source;
        cached.source.outputFolder = tileCacheFolder;
        cached.tz = request.tz;
        cached.tx = request.tx;
        cached.ty = request.ty;
        return TilerHelper::getTile(cached);
    }

    std::string TilerHelper::withTiler(const TileSource &source,
                                       const std::function<std::string(Tiler &)> &render)
    {
        const fs::path &tileablePath = source.path;
        // Files an entry handler tiles itself (COPC point clouds) are read in place; the
        // others are downloaded when remote and tiled by GDAL. Each step checks whether the
        // call was canceled; the GDAL steps also while they run.
        cancel::throwIfCanceled();
        const TilerFactory *factory = findTilerFactory(tileablePath);
        const fs::path localPath = factory != nullptr ? tileablePath : toLocalFile(tileablePath, source.pathHash);
        cancel::throwIfCanceled();

        // A forced recreation must not be served by a tiler prepared before it
        if (source.forceRecreate)
            RasterCache::instance().release(localPath);

        const TilerKey key{io::toUtf8(localPath), source.tileSize, source.tms, io::toUtf8(source.outputFolder)};
        auto tiler = RasterCache::instance().acquireTiler(key, [&]() -> std::unique_ptr<Tiler> {
            if (factory != nullptr)
                return factory->create(localPath, source.outputFolder, source.tileSize, source.tms);

            const fs::path fileToTile = toGeoTIFF(localPath, source.tileSize, source.forceRecreate, "", source.pathHash);
            return std::make_unique<GDALTiler>(io::toUtf8(fileToTile), io::toUtf8(source.outputFolder), source.tileSize,
                                               source.tms);
        });
        cancel::throwIfCanceled();
        return render(*tiler);
    }

    fs::path TilerHelper::getTile(const TileRequest &request)
    {
        const int tz = request.tz, tx = request.tx, ty = request.ty;
        uint8_t **outBuffer = request.outBuffer;
        int *outBufferSize = request.outBufferSize;

        if (request.visParams)
        {
            const ThumbVisParams &visParams = *request.visParams;
            return io::fromUtf8(withTiler(request.source, [&](Tiler &t) {
                // Visualization parameters apply to rasters; point clouds render as usual
                if (auto *raster = dynamic_cast<GDALTiler *>(&t))
                    return raster->tile(tz, tx, ty, visParams, outBuffer, outBufferSize);
                return t.tile(tz, tx, ty, outBuffer, outBufferSize);
            }));
        }

        const ImageFormat format = parseImageFormat(request.format);
        return io::fromUtf8(withTiler(request.source, [&](Tiler &t) {
            return t.tile(tz, tx, ty, format, outBuffer, outBufferSize);
        }));
    }

    fs::path TilerHelper::toLocalFile(const fs::path &tileablePath, const std::string &tileablePathHash)
    {
        fs::path localTileablePath;

        if (io::isNetworkPath(io::toUtf8(tileablePath)))
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
                    const fs::path tmpPath = io::concat(localTileablePath, ".download-" + randomString(16));
                    HttpResponse res;
                    bool writeOk = false;
                    {
                        std::ofstream of(tmpPath, std::ios::binary);
                        HttpRequest request;
                        request.url = io::toUtf8(tileablePath);
                        res = defaultHttpClient().download(request, of);
                        // A disk-full or I/O failure stops the transfer, and also shows on the
                        // stream state
                        of.close();
                        writeOk = !of.fail();
                    }

                    if (!writeOk || res.hasTransportError() || res.status < 200 || res.status >= 300)
                    {
                        io::assureIsRemoved(tmpPath);
                        throw NetException("Cannot download " + io::toUtf8(tileablePath) + " (HTTP " +
                                           std::to_string(res.status) + "): " +
                                           (writeOk ? res.transportMessage : std::string("write error")));
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
            currentUnixTimestamp() - 60 * 60 * 24 * 5; // 5 days
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

    void TilerHelper::runTiler(const fs::path &input, const fs::path &output, std::ostream &os,
                               const TilerRun &run)
    {
        requireOutputFormat(run.format, {OutputFormat::Text, OutputFormat::Json});

        // Owned: parseZRange, std::stoi and tile() throw on bad input or failed tiles
        std::unique_ptr<Tiler> tiler;

        if (const TilerFactory *factory = findTilerFactory(input))
        {
            // Tiled by its entry handler (COPC point cloud)
            tiler = factory->create(input, output, run.tileSize, run.tms);
        }
        else
        {
            // Assume image/geotiff
            fs::path geotiff = ddb::TilerHelper::toGeoTIFF(input, run.tileSize, true);
            tiler = std::make_unique<GDALTiler>(io::toUtf8(geotiff), io::toUtf8(output), run.tileSize, run.tms);
        }

        BoundingBox<int> zb;
        if (run.zRange == "auto")
        {
            zb = tiler->getMinMaxZ();
        }
        else
        {
            zb = parseZRange(run.zRange);
        }

        // One tile per line, or one JSON string per member of an array (escaped, separated across
        // zoom levels as well)
        std::optional<JsonArrayWriter> asJson;
        if (run.format == OutputFormat::Json)
            asJson.emplace(os);
        const auto write = [&](const std::string &tilePath)
        {
            if (asJson)
                asJson->write(tilePath);
            else
                os << tilePath << std::endl;
        };

        for (int z = zb.min; z <= zb.max; z++)
        {
            if (run.x != "auto" && run.y != "auto")
            {
                // Just one tile
                write(tiler->tile(z, std::stoi(run.x), std::stoi(run.y)));
            }
            else
            {
                // All tiles
                std::vector<ddb::TileInfo> tiles = tiler->getTilesForZoomLevel(z);
                for (auto &t : tiles)
                {
                    LOGD << "Tiling " << t.tx << " " << t.ty << " " << t.tz;
                    write(tiler->tile(t));
                }
            }
        }

        if (asJson)
            asJson->finish();
    }

} // namespace ddb
