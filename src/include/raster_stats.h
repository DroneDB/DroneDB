/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_STATS_H
#define RASTER_STATS_H

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "exceptions.h"
#include "fs.h"
#include "gdal_inc.h"

namespace ddb
{

    /// No pixel of the band (or of the sample read for it) has a value: all NoData, NaN or masked. A GDALException,
    /// named so that a render can tell it from a failed read and paint nothing instead of failing. sampled() says
    /// that only a sample of the band was read (getStretchStatistics on a large band): the band may still have
    /// pixels with a value outside it, so the result is not known to be empty
    class NoValidPixelsException : public GDALException
    {
    public:
        explicit NoValidPixelsException(const std::string &message, bool sampled = false)
            : GDALException(message), sampled_(sampled) {}

        /// True when the pixels read were a sample of the band (an overview or some of its blocks), false when
        /// every pixel of the band was read and none has a value
        bool sampled() const noexcept { return sampled_; }

    private:
        bool sampled_;
    };

    /// The statistics of the values of a band, over its pixels that have a value (not NoData, not
    /// masked, finite), as the band holds them: raw samples, before the scale and the offset of the
    /// band. All of them exact (every pixel was read).
    struct BandStatistics
    {
        double min = 0.0;
        double max = 0.0;
        double mean = 0.0;
        double stdDev = 0.0;
        uint64_t validCount = 0; // pixels with a value; 0 when the source of the statistics does not know it
        uint64_t pixelCount = 0; // width x height of the band, 0 when not known
        // Read from a sample of at most the stretch budget of pixels (getStretchStatistics): then
        // validCount counts the sampled pixels that have a value. False for exact statistics.
        bool sampled = false;
    };

    /// The most pixels of a band the default stretch of a render reads when the exact statistics of
    /// the band are not known yet (2048 x 2048): a thumbnail runs in the confined gdal-util worker,
    /// under a cpu time limit, and the exact pass over the bands of a large raster (every block of the
    /// file decoded) does not fit in it.
    inline constexpr uint64_t kStretchStatisticsMaxPixels = uint64_t{1} << 22;

    /**
     * Band statistics used to stretch non-Byte rasters (tiles, thumbnails), to describe a raster (value
     * info, contours) and to size an export (FR-RAS-07, FR-XC-24, D-041), computed at most once per
     * version of a file.
     *
     * The statistics are always exact: a pass over every pixel (bApproxOK FALSE: never an overview or a
     * sample of the blocks), excluding the pixels without a value. They are keyed by the version of the
     * file, the band and the effective NoData value (the override of the caller, else the NoData value
     * of the band). For a key it returns, in order: the statistics of the "<raster>.stats.json" sidecar
     * when it holds an exact entry for the key (a sidecar of an earlier build that did not say they
     * were exact is ignored and replaced), else the exact statistics it computes. A computed entry is
     * kept in memory per file version, and in the sidecar when the raster is a file libddb wrote (an
     * artefact: the COG of a build, a normalized copy), so a rebuilt raster gets new statistics and
     * concurrent callers of a missing key wait for one computation instead of repeating it. Never in
     * PAM files, and never next to the file of a user.
     */
    class RasterStatsProvider
    {
    public:
        enum class Source
        {
            Sidecar,  // <raster>.stats.json
            Computed  // computed from the raster pixels
        };

        struct Counters
        {
            uint64_t fromSidecar = 0;
            uint64_t computed = 0;
            uint64_t sampled = 0; // bands whose stretch statistics were read from a sample
        };

        static RasterStatsProvider &instance();

        /// Statistics of band (1-based) of the raster at path; ds is that raster, open (used only
        /// when the statistics are not cached yet). `nodata` overrides the NoData value of the band
        /// (FR-RAS-31). `persist` keeps a computed entry in the sidecar of ds even when it is not
        /// a file libddb wrote (the COG build, which writes it before it publishes the COG).
        /// @throws NoValidPixelsException (a GDALException) when no pixel of the band has a value
        BandStatistics get(const std::string &path, GDALDatasetH ds, int band,
                           const std::optional<double> &nodata = std::nullopt, bool persist = false);

        /// Statistics for the default stretch (D-041) of a render of `bands` (1-based) of the raster at
        /// path, one per band in the order of `bands`, with a bounded read: the exact statistics when
        /// they are known (in memory, in the sidecar), else read in one pass over all the bands that
        /// lack them (a pixel-interleaved file is decoded once, not once per band). That pass is exact
        /// (and kept as get() keeps it) when a band has at most `maxPixels` pixels; for a larger band it
        /// reads at most `maxPixels` pixels of each band (sampleBandStatistics: an overview, else whole
        /// blocks spread over the raster), and the result, marked sampled, is kept in memory only: never
        /// in the sidecar, never returned by get(). A sample in which no pixel has a value is read once
        /// more with 16 times the budget (exact when the band fits in it). Value info, contours and the
        /// build use get().
        /// @throws NoValidPixelsException (a GDALException) when no pixel of a band has a value: sampled() when
        /// that second read was still a sample (a band of more than 16 times the budget), so the band may have
        /// pixels with a value that the sample missed
        std::vector<BandStatistics> getStretchStatistics(const std::string &path, GDALDatasetH ds,
                                                         const std::vector<int> &bands,
                                                         const std::optional<double> &nodata = std::nullopt,
                                                         uint64_t maxPixels = kStretchStatisticsMaxPixels);

        /// How the statistics returned so far were obtained. Tests read it to check that the
        /// statistics of a raster are computed once and that a sidecar is used when there is one.
        Counters counters() const;

        /// Forgets every cached file (tests start from an empty cache with it)
        void clear();

    private:
        struct Entry;

        RasterStatsProvider() = default;
        std::shared_ptr<Entry> entryFor(const std::string &path);

        mutable std::mutex mutex_;
        std::list<std::shared_ptr<Entry>> entries_; // most recently used first
        Counters counters_;
    };

    /// The statistics sidecar of a raster: "<raster>.stats.json", next to it
    fs::path statsSidecarOf(const fs::path &raster);

    /// Reads the statistics of band (1-based) from the "<raster>.stats.json" sidecar next to path,
    /// for the effective NoData value `nodata` (null: the one of the band). False when there is no
    /// sidecar, it is older than the raster, it does not say its statistics are exact (the sidecar
    /// of a build of before version 2) or it has no entry for the key.
    bool readStatsSidecar(const std::string &path, int band, BandStatistics &out,
                          const std::optional<double> &nodata = std::nullopt);

    /// Writes the entry for (band, nodata) into the sidecar of path, keeping the entries of the
    /// other keys that the sidecar already has (a sidecar that is stale or not exact is replaced).
    /// The file appears whole, by a rename. False (logged) when it cannot be written.
    bool writeStatsSidecar(const std::string &path, int band, const BandStatistics &stats,
                           const std::optional<double> &nodata = std::nullopt);

    /// The exact statistics of the band (a pass over every pixel; the pixels without a value are left
    /// out): the NoData value `nodata` instead of the one of the band when it is set, the alpha band or
    /// mask of the dataset, and every sample that is not a number. The call can be canceled.
    /// @throws GDALException when no pixel has a value
    BandStatistics computeExactStatistics(GDALRasterBandH band, const std::optional<double> &nodata);

    /// The exact statistics of several bands (1-based) of ds, as computeExactStatistics computes them
    /// for one, read in one pass: a file whose blocks hold every band (pixel interleaving) is decoded
    /// once, not once per band. The call can be canceled.
    /// @throws NoValidPixelsException (a GDALException) when no pixel of a band has a value
    std::vector<BandStatistics> computeBandStatistics(GDALDatasetH ds, const std::vector<int> &bands,
                                                      const std::optional<double> &nodata);

    /// The statistics of several bands (1-based) of ds from at most maxPixels pixels of each band, read in
    /// one pass: the exact ones (computeBandStatistics) when a band has no more pixels than that; else
    /// the pixels of the largest overview that has no more (every band has it), else those of whole blocks
    /// of the raster spread on an even lattice, so that only the blocks read are decoded. Those are
    /// marked sampled, and their validCount counts the pixels read that have a value. The call can be
    /// canceled.
    /// @throws NoValidPixelsException (a GDALException) when no pixel of a band has a value, sampled() when
    /// the pixels read were a sample
    std::vector<BandStatistics> sampleBandStatistics(GDALDatasetH ds, const std::vector<int> &bands,
                                                     const std::optional<double> &nodata, uint64_t maxPixels);
}

#endif // RASTER_STATS_H
