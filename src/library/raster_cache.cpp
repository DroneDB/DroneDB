/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_cache.h"

#include "copc_node_cache.h"

#include <algorithm>
#include <cstdlib>

#include "env.h"
#include "exceptions.h"
#include "fileversion.h"
#include "gdal_open_policy.h"
#include "gdal_raii.h"
#include "logger.h"
#include "raster_utils.h"
#include "tiler.h"
#include "mio.h"

namespace ddb
{
    namespace
    {
        // Idle instances are closed at most this often when they expire
        constexpr std::chrono::seconds PurgeInterval{5};

        std::string tilerKey(const TilerKey &key, const std::string &path)
        {
            return "tiler|" + std::to_string(key.tileSize) + "|" + (key.tms ? "tms" : "xyz") + "|" +
                   key.outputFolder + "|" + path;
        }
    }

    OpenedDataset::OpenedDataset(const std::string &path, Kind kind)
    {
        const bool raster = kind == Kind::Raster;
        dataset_ = raster ? openUserRaster(path) : openUserVector(path);
        if (!dataset_)
            throw GDALException(std::string(raster ? "Cannot open raster: " : "Cannot open vector: ") + path);
    }

    OpenedDataset::~OpenedDataset() = default;

    double OpenedDataset::alphaOpaque(int band) const
    {
        if (alphaBand_ != band)
        {
            alphaOpaque_ = alphaOpaqueValue(GDALGetRasterBand(handle(), band));
            alphaBand_ = band;
        }
        return alphaOpaque_;
    }

    RasterCache &RasterCache::instance()
    {
        // Never destroyed: GDAL handles must not be closed during static destruction,
        // after GDAL or PROJ state may be gone. The OS closes them at exit.
        static auto *cache = new RasterCache();
        return *cache;
    }

    RasterCache::RasterCache() : lastPurge_(std::chrono::steady_clock::now())
    {
        options_.perRaster = env::readSize("DDB_RASTER_CACHE_SIZE", options_.perRaster);
        options_.total = env::readSize("DDB_RASTER_CACHE_MAX", options_.total);
        options_.idleTimeout = std::chrono::seconds(
            env::readSize("DDB_RASTER_CACHE_IDLE_SECONDS", static_cast<size_t>(options_.idleTimeout.count())));
    }

    RasterCache::Lease<Tiler> RasterCache::acquireTiler(const TilerKey &key, const TilerFactory &create)
    {
        const auto version = fileVersion(io::fromUtf8(key.path));
        if (version.empty())
        {
            // Not a local file (or gone): nothing to key on, build one for this call only
            return Lease<Tiler>(nullptr, "", "", "", std::shared_ptr<Tiler>(create()));
        }

        const auto path = cacheKeyPath(io::fromUtf8(key.path));
        const auto k = tilerKey(key, path);
        if (auto idle = take(k, version))
            return Lease<Tiler>(this, k, path, version, std::static_pointer_cast<Tiler>(idle));

        std::shared_ptr<Tiler> tiler(create());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.created++;
        }
        return Lease<Tiler>(this, k, path, version, std::move(tiler));
    }

    RasterCache::Lease<OpenedDataset> RasterCache::acquireDataset(const std::string &rasterPath)
    {
        return acquireOpened(rasterPath, OpenedDataset::Kind::Raster);
    }

    RasterCache::Lease<OpenedDataset> RasterCache::acquireVectorDataset(const std::string &vectorPath)
    {
        return acquireOpened(vectorPath, OpenedDataset::Kind::Vector);
    }

    RasterCache::Lease<OpenedDataset> RasterCache::acquireOpened(const std::string &file, OpenedDataset::Kind kind)
    {
        const auto version = fileVersion(io::fromUtf8(file));
        if (version.empty())
            return Lease<OpenedDataset>(nullptr, "", "", "", std::make_shared<OpenedDataset>(file, kind));

        const auto path = cacheKeyPath(io::fromUtf8(file));
        const auto k = (kind == OpenedDataset::Kind::Raster ? "dataset|" : "vector|") + path;
        if (auto idle = take(k, version))
            return Lease<OpenedDataset>(this, k, path, version, std::static_pointer_cast<OpenedDataset>(idle));

        auto dataset = std::make_shared<OpenedDataset>(file, kind);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.created++;
        }
        return Lease<OpenedDataset>(this, k, path, version, std::move(dataset));
    }

    std::shared_ptr<void> RasterCache::take(const std::string &key, const std::string &version)
    {
        std::vector<std::shared_ptr<void>> stale;
        std::shared_ptr<void> found;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!enabledLocked())
                return nullptr;

            collect(stale, std::chrono::steady_clock::now());

            for (auto it = idle_.begin(); it != idle_.end();)
            {
                if (it->key != key)
                {
                    ++it;
                    continue;
                }

                if (it->version != version)
                {
                    // The file was rebuilt or replaced since this instance opened it
                    stale.push_back(std::move(it->object));
                    stats_.discarded++;
                    it = idle_.erase(it);
                    continue;
                }

                found = std::move(it->object);
                idle_.erase(it);
                stats_.reused++;
                break;
            }
        }
        stale.clear(); // Closes outside the lock
        return found;
    }

    void RasterCache::giveBack(const std::string &key, const std::string &path, const std::string &version,
                               std::shared_ptr<void> object)
    {
        // An instance that saw its file change while in use is not kept
        const bool current = fileVersion(io::fromUtf8(path)) == version;

        std::vector<std::shared_ptr<void>> stale;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!current || !enabledLocked())
            {
                stats_.discarded++;
                stale.push_back(std::move(object));
            }
            else
            {
                const auto now = std::chrono::steady_clock::now();
                idle_.push_front(Idle{key, path, version, now, std::move(object)});
                collect(stale, now);
            }
        }
    }

    size_t RasterCache::release(const fs::path &path)
    {
        const auto prefix = cacheKeyPath(path);
        std::vector<std::shared_ptr<void>> closing;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = idle_.begin(); it != idle_.end();)
            {
                if (isCacheKeyUnder(it->path, prefix))
                {
                    closing.push_back(std::move(it->object));
                    it = idle_.erase(it);
                }
                else
                    ++it;
            }
            stats_.discarded += closing.size();
        }
        return closing.size();
    }

    size_t RasterCache::clear()
    {
        std::vector<std::shared_ptr<void>> closing;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto &i : idle_)
                closing.push_back(std::move(i.object));
            idle_.clear();
            stats_.discarded += closing.size();
        }
        return closing.size();
    }

    void RasterCache::configure(const Options &options)
    {
        std::vector<std::shared_ptr<void>> stale;
        std::lock_guard<std::mutex> lock(mutex_);
        options_ = options;
        if (!enabledLocked())
        {
            for (auto &i : idle_)
                stale.push_back(std::move(i.object));
            stats_.discarded += idle_.size();
            idle_.clear();
            return;
        }
        collect(stale, std::chrono::steady_clock::now());
    }

    RasterCache::Options RasterCache::options() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return options_;
    }

    RasterCache::Stats RasterCache::stats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto s = stats_;
        s.idle = idle_.size();
        return s;
    }

    void RasterCache::collect(std::vector<std::shared_ptr<void>> &out, std::chrono::steady_clock::time_point now)
    {
        const size_t before = out.size();

        // Expired (checked every PurgeInterval, or more often for a shorter timeout)
        if (now - lastPurge_ >= std::min<std::chrono::steady_clock::duration>(PurgeInterval, options_.idleTimeout))
        {
            lastPurge_ = now;
            for (auto it = idle_.begin(); it != idle_.end();)
            {
                if (now - it->since >= options_.idleTimeout)
                {
                    out.push_back(std::move(it->object));
                    it = idle_.erase(it);
                }
                else
                    ++it;
            }
        }

        // Per raster and kind: keep the most recently given back
        std::vector<std::pair<const std::string *, size_t>> counts;
        for (auto it = idle_.begin(); it != idle_.end();)
        {
            auto c = std::find_if(counts.begin(), counts.end(), [&](const auto &p) { return *p.first == it->key; });
            if (c == counts.end())
            {
                counts.emplace_back(&it->key, 1);
                ++it;
            }
            else if (++c->second > options_.perRaster)
            {
                out.push_back(std::move(it->object));
                it = idle_.erase(it);
            }
            else
                ++it;
        }

        // In total: drop the least recently given back
        while (idle_.size() > options_.total)
        {
            out.push_back(std::move(idle_.back().object));
            idle_.pop_back();
        }

        stats_.discarded += out.size() - before;
    }

    size_t releaseCachedFiles(const fs::path &path)
    {
        CopcNodeCache::instance().release(path);
        return RasterCache::instance().release(path);
    }
}
