/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_CACHE_H
#define RASTER_CACHE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fs.h"
#include "gdal_inc.h"

namespace ddb
{
    class Tiler;

    /// A raster or vector file opened read-only with GDAL, closed when destroyed
    class OpenedDataset
    {
        GDALDatasetUniquePtr dataset_;

    public:
        enum class Kind
        {
            Raster,
            Vector
        };

        explicit OpenedDataset(const std::string &path, Kind kind = Kind::Raster);
        ~OpenedDataset();
        OpenedDataset(const OpenedDataset &) = delete;
        OpenedDataset &operator=(const OpenedDataset &) = delete;

        GDALDatasetH handle() const { return GDALDataset::ToHandle(dataset_.get()); }
    };

    /// What a cached tiler was built for: the same file tiled with other settings is
    /// another tiler
    struct TilerKey
    {
        std::string path;         // file to tile (a local path)
        int tileSize = 256;
        bool tms = false;
        std::string outputFolder; // empty for in-memory tiles
    };

    /**
     * Process-wide cache of prepared rasters: tilers (GDALTiler, PointCloudTiler) and
     * read-only GDAL datasets (rasters, and the vector GeoPackages that WFS/OGC API
     * Features queries read), keyed by file and kept idle between calls.
     *
     * A tile request on a raster seen recently skips the dataset open, the SRS parsing,
     * the warped VRT and the COPC header read, and keeps GDAL's block cache warm. Objects
     * are never shared: acquire hands each caller its own instance (GDAL handles must not
     * be used by two threads at once) and the Lease gives it back. An instance is reused
     * only while the file keeps the version (fileVersion) it was opened at, so a rebuilt
     * or replaced file is opened again.
     *
     * Replacing or deleting files of this process while they are idle here requires
     * release() first on Windows, where open files cannot be deleted (DDBReleaseConnections
     * in the C API releases these with the index connections). Builds release their output
     * folder before replacing it.
     *
     * Configuration (read once, from the environment):
     *   DDB_RASTER_CACHE_SIZE          idle instances kept per raster and kind (default 8, 0 disables)
     *   DDB_RASTER_CACHE_MAX           idle instances kept in total (default 64)
     *   DDB_RASTER_CACHE_IDLE_SECONDS  idle instances are closed after this (default 60)
     */
    class RasterCache
    {
    public:
        struct Options
        {
            size_t perRaster = 8;
            size_t total = 64;
            std::chrono::seconds idleTimeout{60};
        };

        struct Stats
        {
            size_t idle = 0;
            uint64_t created = 0;   // instances built because none was idle
            uint64_t reused = 0;    // acquisitions served by an idle instance
            uint64_t discarded = 0; // instances closed instead of kept
        };

        /// An instance taken from the cache; given back when the lease is destroyed
        template <typename T>
        class Lease
        {
            RasterCache *cache_ = nullptr;
            std::string key_;
            std::string path_;
            std::string version_;
            std::shared_ptr<T> object_;

        public:
            Lease() = default;
            Lease(RasterCache *cache, std::string key, std::string path, std::string version,
                  std::shared_ptr<T> object)
                : cache_(cache), key_(std::move(key)), path_(std::move(path)), version_(std::move(version)),
                  object_(std::move(object)) {}
            Lease(Lease &&other) noexcept { *this = std::move(other); }
            Lease &operator=(Lease &&other) noexcept
            {
                if (this != &other)
                {
                    giveBack();
                    cache_ = other.cache_;
                    key_ = std::move(other.key_);
                    path_ = std::move(other.path_);
                    version_ = std::move(other.version_);
                    object_ = std::move(other.object_);
                    other.cache_ = nullptr;
                }
                return *this;
            }
            Lease(const Lease &) = delete;
            Lease &operator=(const Lease &) = delete;
            ~Lease() { giveBack(); }

            T *get() const { return object_.get(); }
            T *operator->() const { return object_.get(); }
            T &operator*() const { return *object_; }

        private:
            void giveBack()
            {
                if (cache_ != nullptr && object_)
                    cache_->giveBack(key_, path_, version_, std::static_pointer_cast<void>(std::move(object_)));
                object_.reset();
                cache_ = nullptr;
            }
        };

        using TilerFactory = std::function<std::unique_ptr<Tiler>()>;

        static RasterCache &instance();

        /// A tiler for key: an idle one when available, otherwise one made by create
        Lease<Tiler> acquireTiler(const TilerKey &key, const TilerFactory &create);

        /// The raster at path opened read-only: an idle handle when available
        Lease<OpenedDataset> acquireDataset(const std::string &path);

        /// The vector file at path opened read-only: an idle handle when available. The
        /// caller must clear the filters it sets on its layers before giving it back.
        Lease<OpenedDataset> acquireVectorDataset(const std::string &path);

        /// Closes the idle instances of every file under path (a file, a build folder, a
        /// dataset folder). Returns how many were closed.
        size_t release(const fs::path &path);

        /// Closes every idle instance
        size_t clear();

        /// Replaces the options; closes idle instances beyond the new limits
        void configure(const Options &options);
        Options options() const;
        Stats stats() const;

    private:
        struct Idle
        {
            std::string key;
            std::string path; // cacheKeyPath of the file
            std::string version;
            std::chrono::steady_clock::time_point since;
            std::shared_ptr<void> object;
        };

        RasterCache();

        Lease<OpenedDataset> acquireOpened(const std::string &file, OpenedDataset::Kind kind);
        std::shared_ptr<void> take(const std::string &key, const std::string &version);
        void giveBack(const std::string &key, const std::string &path, const std::string &version,
                      std::shared_ptr<void> object);
        // Under lock: moves expired and over-limit instances to out
        void collect(std::vector<std::shared_ptr<void>> &out, std::chrono::steady_clock::time_point now);
        bool enabledLocked() const { return options_.perRaster > 0 && options_.total > 0; }

        mutable std::mutex mutex_;
        Options options_;
        std::list<Idle> idle_; // most recently given back first
        Stats stats_;
        std::chrono::steady_clock::time_point lastPurge_;
    };

    using TilerLease = RasterCache::Lease<Tiler>;
    using DatasetLease = RasterCache::Lease<OpenedDataset>;

    /// Releases what this process keeps of the files under path (a file or a folder):
    /// idle rasters and tilers (RasterCache) and decoded point-cloud nodes (CopcNodeCache).
    /// Call it before replacing or deleting those files. Returns the rasters closed.
    size_t releaseCachedFiles(const fs::path &path);
}

#endif // RASTER_CACHE_H
