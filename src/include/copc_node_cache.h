/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COPC_NODE_CACHE_H
#define COPC_NODE_CACHE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "copc_utils.h"
#include "fs.h"
#include "point_rasterizer.h"

namespace ddb
{
    /**
     * Process-wide cache of decompressed COPC nodes, shared by every reader.
     *
     * Neighbouring point-cloud tiles read the same shallow nodes (the root alone holds
     * tens of thousands of points), and decompressing them was most of a tile's time.
     * Entries are immutable and handed out as shared pointers; a node requested by several
     * threads at once is decoded once (the others wait for it). The least recently used
     * nodes are dropped beyond the capacity. Entries are keyed by file version, so a
     * rebuilt cloud is never served from old nodes; release() drops the nodes of the files
     * under a folder (DDBReleaseConnections, builds).
     *
     * Configuration (read once): DDB_COPC_NODE_CACHE_MB, capacity in MB (default 256,
     * 0 disables the cache).
     */
    class CopcNodeCache
    {
    public:
        using Node = std::shared_ptr<const PointBatch>;

        static CopcNodeCache &instance();

        /// The decoded node stored under key, loading it with load on a miss. file is the
        /// cloud the node belongs to (for release()). Exceptions of load propagate and
        /// nothing is cached.
        Node get(const std::string &file, const std::string &key, const std::function<Node()> &load);

        /// Drops the nodes of path, or of every file under path when it is a folder
        size_t release(const fs::path &path);

        void clear();
        void setCapacity(size_t bytes);
        size_t bytes() const;

        /// Nodes loaded since start (misses), for tests and diagnostics
        uint64_t loads() const { return loads_.load(); }

    private:
        CopcNodeCache();

        struct Entry
        {
            std::string file;              // cacheKeyPath of the cloud
            std::shared_future<Node> node; // ready once loaded
            size_t bytes = 0;              // 0 while loading
            std::list<std::string>::iterator lru;
        };

        void evict();

        mutable std::mutex mutex_;
        std::unordered_map<std::string, Entry> entries_;
        std::list<std::string> lru_; // most recent first
        size_t bytes_ = 0;
        size_t capacity_;
        std::atomic<uint64_t> loads_{0};
    };

    /// Bytes a decoded node takes in memory
    size_t pointBatchBytes(const PointBatch &points);

    /**
     * Reads the points of a local COPC file for a 2D window and resolution, as
     * readers.copc does with its "bounds" and "resolution" options (same nodes, same
     * points), from the shared CopcNodeCache.
     */
    class CopcNodeReader
    {
        std::string path_;
        std::string cacheFile_;
        std::string version_;
        CopcIndex index_;

    public:
        explicit CopcNodeReader(const std::string &copcPath);

        bool hasColor() const { return index_.hasColor(); }
        const CopcIndex &index() const { return index_; }

        /// Gives sink the points inside [minX, maxX] x [minY, maxY] (cloud CRS, bounds
        /// included) of the nodes that overlap it, down to the depth readers.copc reads for
        /// resolution. Batches are reused between calls of sink.
        void read(double minX, double minY, double maxX, double maxY, double resolution,
                  const std::function<void(PointBatch &)> &sink) const;
    };
}

#endif // COPC_NODE_CACHE_H
