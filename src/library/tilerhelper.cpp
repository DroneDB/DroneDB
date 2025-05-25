/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "tilerhelper.h"
#include "gdaltiler.h"
#include "epttiler.h"
#include "threadlock.h"

#include <memory>
#include <vector>
#include <chrono>
#include <thread>

#include "entry.h"
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
        os << tileablePath.string() << "*" << modifiedTime << "*" << tileSize;
        return Hash::strCRC64(os.str());
    }

    fs::path TilerHelper::getFromUserCache(const fs::path &tileablePath, int tz,
                                           int tx, int ty, int tileSize, bool tms,
                                           bool forceRecreate,
                                           const std::string &tileablePathHash)
    {
        if (!fs::exists(tileablePath))
            throw FSException(tileablePath.string() + " does not exist");
        if (std::rand() % 1000 == 0)
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

    fs::path TilerHelper::getTile(const fs::path &tileablePath, int tz, int tx, int ty, int tileSize, bool tms, bool forceRecreate, const fs::path &outputFolder, uint8_t **outBuffer, int *outBufferSize, const std::string &tileablePathHash)
    {
        if (io::Path(tileablePath).checkExtension({"json"}))
        {
            // Assume EPT
            EptTiler t(tileablePath.string(), outputFolder.string(), tileSize, tms);
            return t.tile(tz, tx, ty, outBuffer, outBufferSize);
        }
        else
        {
            const fs::path fileToTile = toGeoTIFF(tileablePath, tileSize, forceRecreate, "", tileablePathHash);
            GDALTiler t(fileToTile.string(), outputFolder.string(), tileSize, tms);
            return t.tile(tz, tx, ty, outBuffer, outBufferSize);
        }
    }

    fs::path TilerHelper::toGeoTIFF(const fs::path &tileablePath, int tileSize,
                                    bool forceRecreate,
                                    const fs::path &outputGeotiff,
                                    const std::string &tileablePathHash)
    {
        fs::path localTileablePath;

        if (utils::isNetworkPath(tileablePath.string()))
        {
            // Download file to user cache
            const std::string ext = fs::path(tileablePath).extension().string();

            // If we know a priori the hash of the remote resource,
            // we use that value to search our local cache (to avoid
            // downloading things twice)
            bool alwaysDownload = false;
            if (!tileablePathHash.empty())
            {
                localTileablePath = UserProfile::get()->getTilesDir() /
                                    fs::path(tileablePathHash + ext);

                // Download only if not exists
                alwaysDownload = false;
            }
            else
            {
                std::string crc = Hash::strCRC64(tileablePath.string());
                localTileablePath = UserProfile::get()->getTilesDir() / fs::path(crc + ext);
                alwaysDownload = true; // always download, content could have changed
            }

            // One thread at a time
            {
                ThreadLock lock(localTileablePath.string());

                bool download = alwaysDownload || !fs::exists(localTileablePath);
                if (download)
                {
                    std::ofstream of(localTileablePath.string(), std::ios::binary);
                    auto res = cpr::Download(of, cpr::Url(tileablePath.string()));

                    // TODO: Should we check return code?
                    /*if (res.error)
                    {
                        LOGE << "Error downloading " << tileablePath.string() << ": " << res.error.message;
                        io::assureIsRemoved(localTileablePath);
                        throw FSException("Error downloading " + tileablePath.string());
                    }*/
                }
            }
        }
        else
        {
            localTileablePath = tileablePath;
        }

        const EntryType type = fingerprint(localTileablePath);

        if (type == EntryType::GeoRaster)
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
                if (std::rand() % 1000 == 0)
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
                    ThreadLock lock(outputPath.string());

                    // Recheck is needed for other processes that might have generated
                    // the file

                    if (!fs::exists(outputPath))
                    {
                        ddb::geoProject({localTileablePath.string()}, outputPath.string(),
                                        "100%", true);

                        // Helps making sure that output path is available in the filesystem before
                        // releasing the thread lock
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
        Tiler *tiler;

        if (io::Path(input).checkExtension({"json"}))
        {
            // Assume EPT
            tiler = new EptTiler(input.string(), output.string(), tileSize, tms);
        }
        else
        {
            // Assume image/geotiff
            fs::path geotiff = ddb::TilerHelper::toGeoTIFF(input, tileSize, true);
            tiler = new GDALTiler(geotiff.string(), output.string(), tileSize, tms);
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

        delete tiler;
    }

} // namespace ddb
