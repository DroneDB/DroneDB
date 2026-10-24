/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_STATS_H
#define RASTER_STATS_H

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>

#include "gdal_inc.h"

namespace ddb
{
    struct BandStatistics
    {
        double min = 0.0;
        double max = 0.0;
        double mean = 0.0;
        double stdDev = 0.0;
    };

    /**
     * Band statistics used to stretch non-Byte rasters (tiles, thumbnails), computed at
     * most once per version of a file.
     *
     * For a band it returns, in order: the statistics stored in the raster itself, those
     * of the "<raster>.stats.json" sidecar written by the COG build, or approximate
     * statistics computed from the raster (an overview when there is one). Results are
     * kept in memory per file version (fileVersion), never in PAM files, so a rebuilt
     * raster gets new statistics and concurrent callers of a missing band wait for one
     * computation instead of repeating it.
     */
    class RasterStatsProvider
    {
    public:
        enum class Source
        {
            Dataset,  // stored in the raster (GDALGetRasterStatistics without force)
            Sidecar,  // <raster>.stats.json
            Computed  // computed from the raster pixels
        };

        struct Counters
        {
            uint64_t fromDataset = 0;
            uint64_t fromSidecar = 0;
            uint64_t computed = 0;
        };

        static RasterStatsProvider &instance();

        /// Statistics of band (1-based) of the raster at path; ds is that raster, open
        /// (used only when the statistics are not cached yet).
        BandStatistics get(const std::string &path, GDALDatasetH ds, int band);

        /// How the statistics returned so far were obtained (test and diagnostics seam)
        Counters counters() const;

        /// Forgets every cached file
        void clear();

    private:
        struct Entry;

        RasterStatsProvider() = default;
        std::shared_ptr<Entry> entryFor(const std::string &path);

        mutable std::mutex mutex_;
        std::list<std::shared_ptr<Entry>> entries_; // most recently used first
        Counters counters_;
    };

    /// Reads the statistics of band (1-based) from the "<raster>.stats.json" sidecar
    /// next to path. False when there is no sidecar, it is older than the raster or it
    /// has no entry for the band.
    bool readStatsSidecar(const std::string &path, int band, BandStatistics &out);
}

#endif // RASTER_STATS_H
