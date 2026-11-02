/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_stats.h"

#include <fstream>
#include <map>

#include "cancellation.h"
#include "exceptions.h"
#include "fileversion.h"
#include "json.h"
#include "logger.h"
#include "mio.h"

namespace ddb
{
    namespace
    {
        // Files whose statistics are kept (a few doubles per band each)
        constexpr size_t MaxEntries = 256;
    }

    struct RasterStatsProvider::Entry
    {
        std::string path;
        std::string version;
        std::mutex mutex; // held while a band is computed, so callers wait for one computation
        std::map<int, BandStatistics> bands;
    };

    RasterStatsProvider &RasterStatsProvider::instance()
    {
        static RasterStatsProvider provider;
        return provider;
    }

    std::shared_ptr<RasterStatsProvider::Entry> RasterStatsProvider::entryFor(const std::string &path)
    {
        const auto version = fileVersion(io::fromUtf8(path));
        if (version.empty())
            return std::make_shared<Entry>(); // Not a local file: nothing to key a cache on

        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
        {
            if ((*it)->path != path)
                continue;

            if ((*it)->version != version)
            {
                entries_.erase(it);
                break;
            }

            entries_.splice(entries_.begin(), entries_, it);
            return entries_.front();
        }

        auto entry = std::make_shared<Entry>();
        entry->path = path;
        entry->version = version;
        entries_.push_front(entry);
        if (entries_.size() > MaxEntries)
            entries_.pop_back();
        return entry;
    }

    BandStatistics RasterStatsProvider::get(const std::string &path, GDALDatasetH ds, int band)
    {
        const auto entry = entryFor(path);
        std::lock_guard<std::mutex> lock(entry->mutex);

        const auto cached = entry->bands.find(band);
        if (cached != entry->bands.end())
            return cached->second;

        const GDALRasterBandH hBand = GDALGetRasterBand(ds, band);
        if (hBand == nullptr)
            throw GDALException("Cannot read band " + std::to_string(band) + " of " + path);

        BandStatistics s;
        Source source;
        if (GDALGetRasterStatistics(hBand, TRUE, FALSE, &s.min, &s.max, &s.mean, &s.stdDev) == CE_None)
        {
            source = Source::Dataset;
        }
        else if (readStatsSidecar(path, band, s))
        {
            source = Source::Sidecar;
        }
        else
        {
            // A pass over the band (or an overview): it stops when the call is canceled
            cancel::GdalProgress progress;
            if (GDALComputeRasterStatistics(hBand, TRUE, &s.min, &s.max, &s.mean, &s.stdDev,
                                            progress.function(), progress.data()) != CE_None)
            {
                cancel::throwIfCanceled();
                throw GDALException("Cannot compute statistics of band " + std::to_string(band) +
                                    " of " + path);
            }
            source = Source::Computed;
            LOGD << "Computed band " << band << " statistics of " << path << " (" << s.min << ", "
                 << s.max << ")";
        }

        entry->bands[band] = s;

        std::lock_guard<std::mutex> countersLock(mutex_);
        switch (source)
        {
        case Source::Dataset:
            counters_.fromDataset++;
            break;
        case Source::Sidecar:
            counters_.fromSidecar++;
            break;
        case Source::Computed:
            counters_.computed++;
            break;
        }
        return s;
    }

    RasterStatsProvider::Counters RasterStatsProvider::counters() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return counters_;
    }

    void RasterStatsProvider::clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

    fs::path statsSidecarOf(const fs::path &raster)
    {
        return io::concat(raster, ".stats.json");
    }

    bool readStatsSidecar(const std::string &path, int band, BandStatistics &out)
    {
        const fs::path rasterPath = io::fromUtf8(path);
        const fs::path sidecar = statsSidecarOf(rasterPath);

        std::error_code ec;
        const auto sidecarTime = fs::last_write_time(sidecar, ec);
        if (ec)
            return false;
        const auto rasterTime = fs::last_write_time(rasterPath, ec);
        // The COG build writes the sidecar after the raster: an older one describes
        // another raster that was at this path
        if (ec || sidecarTime < rasterTime)
            return false;

        try
        {
            std::ifstream in(sidecar);
            if (!in.is_open())
                return false;
            const auto j = json::parse(in);
            const auto bands = j.find("bands");
            if (bands == j.end())
                return false;
            const auto b = bands->find(std::to_string(band));
            if (b == bands->end())
                return false;

            out.min = b->at("min").get<double>();
            out.max = b->at("max").get<double>();
            out.mean = b->at("mean").get<double>();
            out.stdDev = b->at("std").get<double>();
            return true;
        }
        catch (const std::exception &e)
        {
            LOGW << "Ignoring unreadable statistics sidecar " << io::toUtf8(sidecar) << ": " << e.what();
            return false;
        }
    }
}
