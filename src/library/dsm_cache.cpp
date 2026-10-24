/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dsm_cache.h"

#include "exceptions.h"
#include "logger.h"
#include "mathutil.h"
#include "mio.h"

namespace ddb
{

    std::optional<float> DsmMemoryCache::lookup(double latitude, double longitude)
    {
        const Point2D point(longitude, latitude);
        std::lock_guard<std::mutex> lock(mutex);

        for (auto &it : entries)
        {
            if (it.second.bbox.contains(point))
            {
                // Skip entries whose raster data has not been loaded
                if (it.second.data.empty()) continue;

                float elevation = it.second.getElevation(latitude, longitude);
                if (!ddb::isNodata(elevation, it.second.nodata))
                {
                    return elevation;
                }
                else
                {
                    LOGD << "DSM does not have a value for " << point;
                    return 0.0f;
                }
            }
        }

        return std::nullopt;
    }

    bool DsmMemoryCache::contains(const std::string &filename)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return entries.find(filename) != entries.end();
    }

    void DsmMemoryCache::put(const std::string &filename, DSMCacheEntry entry)
    {
        std::lock_guard<std::mutex> lock(mutex);
        entries[filename] = std::move(entry);
    }

    void DsmMemoryCache::erase(const std::string &filename)
    {
        std::lock_guard<std::mutex> lock(mutex);
        entries.erase(filename);
    }

    bool addRasterToCache(DsmMemoryCache &memory, const fs::path &filePath, double latitude, double longitude)
    {
        DsmRaster raster = readDsmRaster(filePath, latitude, longitude);
        const bool contained = raster.containsPoint;

        // Always cache the entry (at least with bbox) to avoid
        // repeated GDALOpen calls for files outside query bounds
        memory.put(io::toUtf8(filePath.filename()), std::move(raster.entry));
        return contained;
    }

    bool loadDsmFolder(const fs::path &folder, DsmMemoryCache &memory, double latitude, double longitude)
    {
        for (const auto &entry : fs::directory_iterator(folder))
        {
            // Not found in cache?
            const std::string filename = io::toUtf8(entry.path().filename());
            if (!memory.contains(filename))
            {
                LOGD << "Adding " << io::toUtf8(entry.path()) << " to DSM service cache";
                try
                {
                    if (addRasterToCache(memory, entry.path(), latitude, longitude))
                    {
                        return true; // Stop early, we've found a match
                    }
                }
                catch (const GDALException &)
                {
                    LOGD << "Deleting " << io::toUtf8(entry.path()) << " because we can't open it";
                    fs::remove(entry.path());
                    memory.erase(filename);
                }
            }
        }

        return false; // No match
    }

} // namespace ddb
