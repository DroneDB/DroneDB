/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_STATS_H
#define RASTER_STATS_H

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "exceptions.h"
#include "fs.h"
#include "gdal_inc.h"
#include "json_fwd.h"
#include "raster_source.h"
#include "raster_utils.h"

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

    /// Statistics by band (1-based): the bands of a raster that have a pixel with a value
    using StatisticsByBand = std::map<int, BandStatistics>;

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
     * sample of the blocks), excluding the pixels without a value. A raster ddb-tilers reads is passed once
     * for all its bands, on several threads, with the arithmetic of the sidecar of a COG build (exact sums:
     * for integer samples the values of that sidecar); any other source is read row by row. They are keyed by the version of the
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
        BandStatistics get(const std::string &path, const RasterSource &source, int band,
                           const std::optional<double> &nodata = std::nullopt, bool persist = false);
        BandStatistics get(const std::string &path, GDALDatasetH ds, int band,
                           const std::optional<double> &nodata = std::nullopt, bool persist = false)
        {
            return get(path, *rasterSourceOf(ds), band, nodata, persist);
        }

        /// get() for several bands (1-based) of the raster at path, with the same `nodata` and
        /// `persist`: the statistics of the bands that are not known yet (in memory, in the sidecar) are computed
        /// in one pass over all of them, as computeBandStatistics reads them (a file whose blocks hold every band,
        /// pixel interleaving, is decoded once and not once per band), the same to the bit as get() computes
        /// them band by band (each band takes its pixels in the same order), and kept as get() keeps them. One
        /// element per band of `bands`; nullopt for a band in which no pixel has a value (get() throws
        /// NoValidPixelsException for it). `progress`, when set, is told the fraction (0 to 1) of the rows of
        /// the raster read by that pass (1 at once when every band is known). `maxThreads` caps the threads of that
        /// pass over a raster ddb-tilers reads (0: DDB_STATS_THREADS, by default up to 8; the CPU pool decides how
        /// many run). A band found without a pixel with a value is remembered as such. The call can be canceled.
        /// @throws GDALException when a band cannot be read (nothing is kept then)
        std::vector<std::optional<BandStatistics>> getAll(const std::string &path, const RasterSource &source,
                                                          const std::vector<int> &bands,
                                                          const std::optional<double> &nodata, bool persist,
                                                          const std::function<void(double)> &progress = {},
                                                          size_t maxThreads = 0);
        std::vector<std::optional<BandStatistics>> getAll(const std::string &path, GDALDatasetH ds,
                                                          const std::vector<int> &bands,
                                                          const std::optional<double> &nodata, bool persist,
                                                          const std::function<void(double)> &progress = {})
        {
            return getAll(path, *rasterSourceOf(ds), bands, nodata, persist, progress);
        }

        /// Statistics for the default stretch (D-041) of a render of `bands` (1-based) of the raster at
        /// path, one per band in the order of `bands`, with a bounded read: the exact statistics when
        /// they are known (in memory, in the sidecar), else read in one pass over all the bands that
        /// lack them (a pixel-interleaved file is decoded once, not once per band). That pass is exact
        /// (and kept as get() keeps it) when a band has at most `maxPixels` pixels; for a larger band it
        /// reads at most `maxPixels` pixels of each band (sampleBandStatistics: an overview, else whole
        /// blocks spread over the raster), and the result, marked sampled, is kept in memory only: never
        /// in the sidecar, never returned by get(). A sample in which no pixel has a value is read once
        /// more with 16 times the budget (exact when the band fits in it). Value info and contours use
        /// get(), the build getAll().
        /// @throws NoValidPixelsException (a GDALException) when no pixel of a band has a value: sampled() when
        /// that second read was still a sample (a band of more than 16 times the budget), so the band may have
        /// pixels with a value that the sample missed
        std::vector<BandStatistics> getStretchStatistics(const std::string &path, const RasterSource &source,
                                                         const std::vector<int> &bands,
                                                         const std::optional<double> &nodata = std::nullopt,
                                                         uint64_t maxPixels = kStretchStatisticsMaxPixels);
        std::vector<BandStatistics> getStretchStatistics(const std::string &path, GDALDatasetH ds,
                                                         const std::vector<int> &bands,
                                                         const std::optional<double> &nodata = std::nullopt,
                                                         uint64_t maxPixels = kStretchStatisticsMaxPixels)
        {
            return getStretchStatistics(path, *rasterSourceOf(ds), bands, nodata, maxPixels);
        }

        /// How the statistics returned so far were obtained. Tests read it to check that the
        /// statistics of a raster are computed once and that a sidecar is used when there is one.
        Counters counters() const;

        /// Forgets every cached file (tests start from an empty cache with it)
        void clear();

        /// Takes `bands` as the exact statistics of those bands of the raster at path, for the NoData value of
        /// each band (no override), in its version of now: get() and getAll() return them without a pass. The
        /// statistics of a build of the file (readSourceStatistics) that the caller checked are of this file.
        /// A key known already keeps its statistics.
        void adopt(const std::string &path, const StatisticsByBand &bands);

    private:
        struct Entry;

        RasterStatsProvider() = default;
        std::shared_ptr<Entry> entryFor(const std::string &path);

        mutable std::mutex mutex_;
        std::list<std::shared_ptr<Entry>> entries_; // most recently used first
        Counters counters_;
    };

    /// The range GDALComputeRasterMinMax(band, bApproxOK TRUE) gives (what gdal_translate -scale stretches a band
    /// over): the band's STATISTICS_MINIMUM and STATISTICS_MAXIMUM metadata when it has both; else the minimum and
    /// maximum of the overview with the fewest pixels above 2500 (GetRasterSampleOverview), every pixel of it;
    /// else those of every n-th block of the band, n the square root of its blocks (one more when that is the
    /// blocks of a row). The pixels at the NoData value, NaN, and masked (the alpha band or the dataset mask, when
    /// the band has no NoData value and is not alpha) are left out. No pixel left: 0, 0 (+inf, -inf for an
    /// overview of 32 or 64-bit samples without a mask, as GDAL's).
    std::pair<double, double> approximateMinMax(const RasterSource &source, int band);

    /// The statistics sidecar of a raster: "<raster>.stats.json", next to it
    fs::path statsSidecarOf(const fs::path &raster);

    /// The statistics of the input of a COG build, next to the COG: "<cog>.source.stats.json". They are those of
    /// the pixels of the file the COG was built from (the uploaded raster), not of the COG (warped, resampled,
    /// encoded again), with the rules of the exact pass (computeExactStatistics, no NoData override), in the
    /// schema of the sidecar: written by the build in the same pass, or later by DDBBuildRasterStatistics.
    fs::path sourceStatsSidecarOf(const fs::path &cog);

    /// The source statistics of the COG at `cog` (sourceStatsSidecarOf): one per band that has a pixel with a
    /// value. nullopt when there are none that are used: missing, older than the COG, not exact or unreadable.
    std::optional<StatisticsByBand> readSourceStatistics(const fs::path &cog);

    /// Writes `bands` as the source statistics of the COG at `cog` (whole, by a rename), replacing what is there.
    /// False (logged) when they cannot be written.
    bool writeSourceStatistics(const fs::path &cog, const StatisticsByBand &bands);

    /// {"<band>": {min, max, mean, std, valid, total}}, as the "bands" of a sidecar holds them
    json statisticsByBandToJson(const StatisticsByBand &bands);
    /// The reverse of statisticsByBandToJson, for statistics another process wrote: every member is checked
    /// (finite numbers, counts, min <= mean <= max, 0 < valid <= total).
    /// @throws InvalidArgsException when they are not such statistics
    StatisticsByBand statisticsByBandFromJson(const json &bands);

    /// Reads the statistics of band (1-based) from the "<raster>.stats.json" sidecar next to path,
    /// for the effective NoData value `nodata` (null: the one of the band). False when there is no
    /// sidecar, it is older than the raster, it does not say its statistics are exact (the sidecar
    /// of a build of before version 2) or it has no entry for the key.
    bool readStatsSidecar(const std::string &path, int band, BandStatistics &out,
                          const std::optional<double> &nodata = std::nullopt);

    /// Whether the raster has a sidecar of its statistics that is used: not older than the raster, exact (a
    /// sidecar of a COG build, or one written by a pass over its pixels)
    bool hasFreshStatsSidecar(const fs::path &raster);

    /// Writes an exact sidecar without an entry next to path when it has none that is used: a raster in which no
    /// band has a pixel with a value then says that its statistics were computed. False (logged) when it cannot be
    /// written.
    bool writeEmptyStatsSidecar(const std::string &path);

    /// Writes the entry for (band, nodata) into the sidecar of path, keeping the entries of the
    /// other keys that the sidecar already has (a sidecar that is stale or not exact is replaced).
    /// The file appears whole, by a rename. False (logged) when it cannot be written.
    bool writeStatsSidecar(const std::string &path, int band, const BandStatistics &stats,
                           const std::optional<double> &nodata = std::nullopt);

    /// The exact statistics of several bands (1-based) of the source (a pass over every pixel; the pixels without a
    /// value are left out: the NoData value `nodata` instead of the one of the band when it is set, the alpha
    /// band or mask of the dataset, every sample that is not a number), read in one pass: a file whose blocks
    /// hold every band (pixel interleaving) is decoded once, not once per band. The call can be canceled.
    /// @throws NoValidPixelsException (a GDALException) when no pixel of a band has a value
    std::vector<BandStatistics> computeBandStatistics(const RasterSource &source, const std::vector<int> &bands,
                                                      const std::optional<double> &nodata);

    /// computeBandStatistics without the rule that every band has a pixel with a value (nullopt for a band that
    /// has none), on at most `maxThreads` threads for a raster ddb-tilers reads (0: DDB_STATS_THREADS), telling
    /// `progress` the fraction read. Nothing is kept. The call can be canceled.
    std::vector<std::optional<BandStatistics>> computeExactStatistics(const RasterSource &source,
                                                                      const std::vector<int> &bands,
                                                                      const std::optional<double> &nodata,
                                                                      size_t maxThreads = 0,
                                                                      const std::function<void(double)> &progress = {});
    inline std::vector<BandStatistics> computeBandStatistics(GDALDatasetH ds, const std::vector<int> &bands,
                                                             const std::optional<double> &nodata)
    {
        return computeBandStatistics(*rasterSourceOf(ds), bands, nodata);
    }

    /// The pixels that a read of at most a budget of pixels takes of a window of a raster (planBoundedSample):
    /// the bounded read of the stretch of a thumbnail (sampleBandStatistics) and of the automatic range of a
    /// formula export
    struct BoundedSample
    {
        enum class Kind
        {
            Whole,     // every pixel of the window: it has no more than the budget
            Overview,  // the window in the largest overview that every band has with no more pixels than the budget
            Decimated, // the window read at a smaller size within the budget: an even grid of its pixels (nearest),
                       // every block of the window decoded once
            Blocks     // whole blocks of the raster (cut to the window) on an even lattice: only those are decoded
        };
        /// How a window larger than the budget is read when no overview is within it: whole blocks on a lattice
        /// (the fewest blocks decoded, for a read under a time limit), or decimated (an even spread of its pixels,
        /// which follows the values of the whole window more closely, for the price of decoding every block)
        enum class Fallback
        {
            Blocks,
            Decimated
        };
        Kind kind = Kind::Whole;
        RasterWindow part; // the window, in the pixels of the raster
        // Overview: its index (GDALGetOverview)
        int overview = -1;
        // Overview and Decimated: the size the window is read at (in the pixels of the overview)
        int outWidth = 0;
        int outHeight = 0;
        // Blocks: the size of a block, the block of the raster where the window starts, the blocks across and
        // down that the window touches and the step of the lattice (the middle block of each step x step
        // cell of them is read)
        int blockWidth = 0;
        int blockHeight = 0;
        uint64_t firstBlockX = 0;
        uint64_t firstBlockY = 0;
        uint64_t blocksX = 0;
        uint64_t blocksY = 0;
        uint64_t step = 1;

        /// Whether the sample leaves pixels of the window out (all kinds but Whole)
        bool sampled() const { return kind != Kind::Whole; }

        /// Blocks: calls `read` with each block of the lattice cut to the window (at full resolution: the output
        /// of each window is its size), row by row
        void forEachBlock(const std::function<void(const RasterWindow &)> &read) const;

        /// Calls `read` with windows that read the sample, row by row, each over at most maxSide x maxSide pixels
        /// of the raster, so a read decodes no more blocks and fills no larger buffers than a window of that size:
        /// the window cut into such windows (Whole: the windows of a grid of that size from its corner), each
        /// block of the lattice cut the same way (Blocks), or the window at the size it is read at, cut into the
        /// windows of a grid whose cells cover at most that many pixels (Overview, Decimated: each window has
        /// fewer output pixels than pixels, which GDAL reads from the overview or decimates)
        void forEachWindow(int maxSide, const std::function<void(const RasterWindow &)> &read) const;
    };

    /// How a read of at most maxPixels pixels samples the window `part` (in the pixels of the raster) of the
    /// bands (1-based) of the source: every pixel when it has no more than that; else its pixels in the largest overview
    /// that every band has (at the same size) in which it has no more; else, with Fallback::Blocks, those of whole
    /// blocks of the raster (the blocks of the first band) on an even lattice of at most maxPixels / the pixels of a
    /// block (one at least) blocks, so that only the blocks read are decoded, and with Fallback::Decimated the window
    /// read at the largest size of its proportions within the budget. Throws GDALException for a band that the source does
    /// not have.
    BoundedSample planBoundedSample(const RasterSource &source, const std::vector<int> &bands,
                                    const RasterWindow &part, uint64_t maxPixels,
                                    BoundedSample::Fallback fallback = BoundedSample::Fallback::Blocks);
    inline BoundedSample planBoundedSample(GDALDatasetH ds, const std::vector<int> &bands, const RasterWindow &part,
                                           uint64_t maxPixels,
                                           BoundedSample::Fallback fallback = BoundedSample::Fallback::Blocks)
    {
        return planBoundedSample(*rasterSourceOf(ds), bands, part, maxPixels, fallback);
    }

    /// The budget of the second read of a sample in which no pixel has a value (a sparse raster): 16 times
    /// maxPixels, still bounded
    uint64_t sparseSampleBudget(uint64_t maxPixels);

    /// The statistics of several bands (1-based) of ds from at most maxPixels pixels of each band, read in
    /// one pass: the sample of planBoundedSample over the whole raster. Every pixel gives the exact ones
    /// (computeBandStatistics); the pixels of an overview or of the blocks of a lattice are marked sampled,
    /// and their validCount counts the pixels read that have a value. The call can be canceled.
    /// @throws NoValidPixelsException (a GDALException) when no pixel of a band has a value, sampled() when
    /// the pixels read were a sample
    std::vector<BandStatistics> sampleBandStatistics(const RasterSource &source, const std::vector<int> &bands,
                                                     const std::optional<double> &nodata, uint64_t maxPixels);
    inline std::vector<BandStatistics> sampleBandStatistics(GDALDatasetH ds, const std::vector<int> &bands,
                                                            const std::optional<double> &nodata, uint64_t maxPixels)
    {
        return sampleBandStatistics(*rasterSourceOf(ds), bands, nodata, maxPixels);
    }
}

#endif // RASTER_STATS_H
