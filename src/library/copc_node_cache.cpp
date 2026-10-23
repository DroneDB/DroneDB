/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "copc_node_cache.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "env.h"
#include "exceptions.h"
#include "fileversion.h"
#include "logger.h"
#include "parallel.h"
#include "mio.h"

namespace ddb
{
    namespace
    {
        constexpr size_t DefaultCapacityMb = 256;
        constexpr size_t BatchSize = 16384;

        // Nodes of one window decompress on up to this many threads (as readers.copc)
        constexpr size_t DecodeThreads = 4;

        // Up to 1 TiB; 0 turns the cache off
        size_t capacityFromEnvironment()
        {
            const auto mb = env::readIntegerOrWarn("DDB_COPC_NODE_CACHE_MB", 0, 1 << 20);
            return static_cast<size_t>(mb.value_or(DefaultCapacityMb)) << 20;
        }

        std::string nodeKey(const std::string &file, const std::string &version, const CopcNodeKey &k)
        {
            return file + "|" + version + "|" + std::to_string(k.d) + "-" + std::to_string(k.x) + "-" +
                   std::to_string(k.y) + "-" + std::to_string(k.z);
        }
    }

    size_t pointBatchBytes(const PointBatch &points)
    {
        return sizeof(PointBatch) + points.size() * (3 * sizeof(double) + 3 * sizeof(uint16_t));
    }

    CopcNodeCache::CopcNodeCache() : capacity_(capacityFromEnvironment()) {}

    CopcNodeCache &CopcNodeCache::instance()
    {
        static CopcNodeCache cache;
        return cache;
    }

    CopcNodeCache::Node CopcNodeCache::get(const std::string &file, const std::string &key,
                                           const std::function<Node()> &load)
    {
        std::promise<Node> promise;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (capacity_ == 0)
            {
                lock.unlock();
                loads_++;
                return load();
            }

            if (const auto it = entries_.find(key); it != entries_.end())
            {
                lru_.splice(lru_.begin(), lru_, it->second.lru);
                const auto node = it->second.node;
                lock.unlock();
                return node.get(); // waits while another thread loads it
            }

            lru_.push_front(key);
            entries_[key] = Entry{file, promise.get_future().share(), 0, lru_.begin()};
            loads_++;
        }

        Node node;
        try
        {
            node = load();
        }
        catch (...)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (const auto it = entries_.find(key); it != entries_.end() && it->second.bytes == 0)
                {
                    lru_.erase(it->second.lru);
                    entries_.erase(it);
                }
            }
            promise.set_exception(std::current_exception());
            throw;
        }
        promise.set_value(node);

        std::lock_guard<std::mutex> lock(mutex_);
        // Still there unless released or cleared while loading
        if (const auto it = entries_.find(key); it != entries_.end() && it->second.bytes == 0)
        {
            it->second.bytes = pointBatchBytes(*node);
            bytes_ += it->second.bytes;
            evict();
        }
        return node;
    }

    void CopcNodeCache::evict()
    {
        auto it = lru_.end();
        while (bytes_ > capacity_ && it != lru_.begin())
        {
            --it;
            const auto entry = entries_.find(*it);
            if (entry->second.bytes == 0)
                continue; // still loading
            bytes_ -= entry->second.bytes;
            entries_.erase(entry);
            it = lru_.erase(it);
        }
    }

    size_t CopcNodeCache::release(const fs::path &path)
    {
        const std::string prefix = cacheKeyPath(path);
        std::lock_guard<std::mutex> lock(mutex_);
        size_t released = 0;
        for (auto it = entries_.begin(); it != entries_.end();)
        {
            if (isCacheKeyUnder(it->second.file, prefix))
            {
                bytes_ -= it->second.bytes;
                lru_.erase(it->second.lru);
                it = entries_.erase(it);
                released++;
            }
            else
                ++it;
        }
        return released;
    }

    void CopcNodeCache::clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        lru_.clear();
        bytes_ = 0;
    }

    void CopcNodeCache::setCapacity(size_t bytes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_ = bytes;
        evict();
    }

    size_t CopcNodeCache::bytes() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return bytes_;
    }

    CopcNodeReader::CopcNodeReader(const std::string &copcPath)
        : path_(copcPath), cacheFile_(cacheKeyPath(io::fromUtf8(copcPath))), version_(fileVersion(io::fromUtf8(copcPath)))
    {
        if (io::isNetworkPath(copcPath))
            throw InvalidArgsException("CopcNodeReader reads local files only: " + copcPath);
        if (version_.empty())
            throw FSException(copcPath + " does not exist");
        index_ = readCopcIndex(copcPath);
    }

    void CopcNodeReader::read(double minX, double minY, double maxX, double maxY, double resolution,
                              const std::function<void(PointBatch &)> &sink) const
    {
        const int depthEnd = copcDepthEnd(index_.spacing, resolution);

        std::vector<const CopcNode *> selected;
        for (const auto &node : index_.nodes)
        {
            if (node.key.d >= depthEnd)
                continue;
            double nMinX, nMinY, nMaxX, nMaxY;
            index_.nodeBounds(node.key, nMinX, nMinY, nMaxX, nMaxY);
            if (nMinX <= maxX && nMaxX >= minX && nMinY <= maxY && nMaxY >= minY)
                selected.push_back(&node);
        }

        std::vector<CopcNodeCache::Node> nodes(selected.size());
        auto &cache = CopcNodeCache::instance();
        parallel::forEachIndex(selected.size(), DecodeThreads, [&](size_t i) {
            const CopcNode &node = *selected[i];
            nodes[i] = cache.get(cacheFile_, nodeKey(cacheFile_, version_, node.key), [&]() {
                auto points = std::make_shared<PointBatch>();
                decodeCopcNode(path_, index_, node, *points);
                return CopcNodeCache::Node(std::move(points));
            });
        });

        PointBatch batch;
        batch.reserve(BatchSize);
        for (const auto &node : nodes)
        {
            const PointBatch &p = *node;
            for (size_t i = 0; i < p.size(); i++)
            {
                if (p.x[i] < minX || p.x[i] > maxX || p.y[i] < minY || p.y[i] > maxY)
                    continue;
                batch.x.push_back(p.x[i]);
                batch.y.push_back(p.y[i]);
                batch.z.push_back(p.z[i]);
                batch.red.push_back(p.red[i]);
                batch.green.push_back(p.green[i]);
                batch.blue.push_back(p.blue[i]);
                if (batch.size() == BatchSize)
                {
                    sink(batch);
                    batch.clear();
                }
            }
        }
        if (batch.size() > 0)
            sink(batch);
    }
}
