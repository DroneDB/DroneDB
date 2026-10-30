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
#include "cog.h"
#include "geoproject.h"
#include "raster_artefacts.h"
#include "raster_helper.h"
#include "raster_stats.h"
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

            const fs::path fileToTile = toGeoTIFF(localPath, source.forceRecreate, source.pathHash);
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

    namespace
    {
        // Removes the files of `folder` whose names start with `prefix`, except `keep`. A file
        // that cannot go now (Windows: still open) is left for the next version.
        void removeEarlierVersions(const fs::path &folder, const std::string &prefix, const fs::path &keep)
        {
            std::error_code ec;
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
            {
                const fs::path &file = it->path();
                if (file == keep || !io::toUtf8(file.filename()).starts_with(prefix))
                    continue;
                std::error_code removeError;
                if (it->is_regular_file(removeError) && !fs::remove(file, removeError) && removeError)
                {
                    LOGD << "Cannot remove the earlier download " << io::toUtf8(file) << ": " << removeError.message();
                }
            }
        }
    } // namespace

    fs::path TilerHelper::toLocalFile(const fs::path &tileablePath, const std::string &tileablePathHash)
    {
        const std::string url = io::toUtf8(tileablePath);
        if (!io::isNetworkPath(url))
            return tileablePath;

        const fs::path tilesDir = UserProfile::get()->getTilesDir();
        const std::string extension = raster::urlFileExtension(url);

        if (!tileablePathHash.empty())
        {
            // The caller knows the hash of the remote file: a copy of that name is the file, and
            // it is downloaded (one thread at a time) only when there is none. A failed transfer
            // never lands in the cache.
            const fs::path local = tilesDir / io::fromUtf8(tileablePathHash + extension);
            ThreadLock lock(io::toUtf8(local));
            if (!fs::exists(local))
            {
                const fs::path download = io::concat(local, ".download-" + randomString(16));
                raster::downloadInput(url, download);
                io::rename(download, local);
            }
            return local;
        }

        // The remote file may have changed since the last call, so it is downloaded every time,
        // and kept under the URL and the digest of its content. An unchanged file keeps its name
        // and its state, and so what was made of it (the artefact, keyed by the path and the
        // state, and the tilers kept open on it): the download is dropped. A changed one gets a
        // name of its own, so no file is ever replaced under a reader, and the copies of the
        // earlier versions of the URL go.
        const std::string urlPrefix = Hash::strCRC64(url) + "-";
        const fs::path download = tilesDir / io::fromUtf8("download-" + randomString(16) + extension);
        raster::downloadInput(url, download);
        try
        {
            const fs::path local =
                tilesDir / io::fromUtf8(urlPrefix + Hash::fileSHA256(io::toUtf8(download)) + extension);
            ThreadLock lock(io::toUtf8(local));
            if (fs::exists(local))
            {
                io::assureIsRemoved(download);
            }
            else
            {
                io::rename(download, local);
                removeEarlierVersions(tilesDir, urlPrefix, local);
            }
            return local;
        }
        catch (...)
        {
            io::assureIsRemoved(download);
            throw;
        }
    }

    namespace
    {
        // The artefact `target` for a tiler: the one that is there, else the one `produce` makes
        // (a forced call always makes it). One thread at a time makes it, and the file appears
        // whole, by a rename.
        fs::path artefactOnce(const fs::path &target, bool forceRecreate, const std::function<void()> &produce)
        {
            std::error_code ec;
            if (!forceRecreate && fs::exists(target))
            {
                // A file in use stays: the cleanup removes what was not touched for days. A
                // statistics sidecar next to it is touched with it, since it counts only while it
                // is not older than the raster (raster_stats.h)
                const auto now = fs::file_time_type::clock::now();
                fs::last_write_time(target, now, ec);
                fs::last_write_time(statsSidecarOf(target), now, ec);
                return target;
            }

            ThreadLock lock(io::toUtf8(target));

            // Another thread may have made it while we waited (it is published by a rename)
            if (forceRecreate || !fs::exists(target))
                produce();
            return target;
        }

        // Closes the idle tilers that read `artefact`, the file made from `source`, before it is
        // replaced: Windows replaces no file a process holds open. A tiler is cached under the
        // path it was asked for (the file of the user), not the artefact it opened, so both go.
        void releaseTilersOf(const fs::path &source, const fs::path &artefact)
        {
            RasterCache::instance().release(source);
            RasterCache::instance().release(artefact);
        }

        // The COG of a raster of a user, in the artefacts of the cache: built by the raster
        // helper, checked, and moved to the name the raster and its state give. Its statistics
        // stay behind: the tiles are stretched with statistics computed on the whole COG, as
        // they are for any raster without a sidecar, not with the approximate ones of a build.
        void produceCogArtefact(const fs::path &raster, const fs::path &target)
        {
            const io::ScopedTempDir request = raster::makeArtefactRequestFolder("cog-");
            const fs::path produced = request.path() / "cog.tif";
            // The build refuses an output that does not open as a raster
            buildCogInPrivateFolder(io::toUtf8(raster), produced);

            releaseTilersOf(raster, target);
            raster::publishArtefact(produced, target);
        }
    } // namespace

    fs::path TilerHelper::toGeoTIFF(const fs::path &tileablePath, bool forceRecreate,
                                    const std::string &tileablePathHash)
    {
        const fs::path localTileablePath = toLocalFile(tileablePath, tileablePathHash);

        // A file libddb wrote (the COG of a build) is tiled as it is
        if (raster::isLibddbArtefact(localTileablePath))
            return localTileablePath;

        if (oneIn(1000))
            cleanupUserCache();

        // The tiler reads only what libddb wrote, so a file of a user (or a download) is read
        // once, by the raster helper, and the tiles come from what it writes
        const fs::path key = raster::artefactsDir() / raster::artefactKey(localTileablePath);
        if (entryHandler(fingerprint(localTileablePath)).tilesWithoutReprojection())
        {
            // Georasters are converted to a COG
            const fs::path target = io::concat(key, ".tif");
            return artefactOnce(target, forceRecreate, [&] { produceCogArtefact(localTileablePath, target); });
        }

        // Other images are geoprojected first. Multiple threads could be generating the
        // geoprojected file at the same time, so artefactOnce places a lock (geoProject
        // publishes it via an atomic rename)
        const fs::path target = io::concat(key, "-geoprojected.tif");
        return artefactOnce(target, forceRecreate, [&] {
            releaseTilersOf(localTileablePath, target);
            ddb::geoProject({io::toUtf8(localTileablePath)}, io::toUtf8(target), "100%", true);
        });
    }

    namespace
    {
        // Removes the folders below `folder` that were not touched since `threshold`, the oldest
        // level first (what is below a folder that goes is not looked at). The cache is shared by
        // every process of the user and by the tile requests of this one, which make and remove
        // folders at the same time, and so does another clean-up: a folder that was listed may be
        // gone when it is looked at. That is the normal state of the cache, so the listing of a
        // folder is taken whole before anything in it is removed, and what vanished is skipped.
        void removeStaleFolders(const fs::path &folder, time_t threshold)
        {
            std::vector<fs::path> subfolders;
            std::error_code ec;
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
            {
                std::error_code statEc;
                if (it->symlink_status(statEc).type() == fs::file_type::directory && !statEc)
                    subfolders.push_back(it->path());
            }

            for (const auto &subfolder : subfolders)
            {
                std::error_code removeEc;
                try
                {
                    if (io::Path(subfolder).getModifiedTime() >= threshold)
                    {
                        removeStaleFolders(subfolder, threshold);
                        continue;
                    }
                }
                catch (const FSException &)
                {
                    continue; // removed by somebody else since the listing
                }
                fs::remove_all(subfolder, removeEc);
                if (removeEc)
                {
                    LOGD << "Cannot remove " << io::toUtf8(subfolder) << " from the cache: " << removeEc.message();
                }
            }
        }
    } // namespace

    void TilerHelper::cleanupUserCache()
    {
        LOGD << "Cleaning up tiles user cache";

        constexpr time_t maxAge = 60 * 60 * 24 * 5; // 5 days
        const time_t threshold = currentUnixTimestamp() - maxAge;

        // A housekeeping done on the way of a request must not fail the request: whatever goes
        // wrong here is logged and the next clean-up looks again
        try
        {
            // What the raster helper wrote for the tiles: the same rule
            raster::cleanupArtefacts(maxAge);
            removeStaleFolders(UserProfile::get()->getTilesDir(), threshold);
        }
        catch (const std::exception &e)
        {
            LOGW << "Cannot clean up the tiles user cache: " << e.what();
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
            fs::path geotiff = ddb::TilerHelper::toGeoTIFF(input);
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
