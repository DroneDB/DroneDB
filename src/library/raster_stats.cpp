/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_stats.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

#include "cancellation.h"
#include "exceptions.h"
#include "fileversion.h"
#include "json.h"
#include "logger.h"
#include "mio.h"
#include "raster_artefacts.h"
#include "raster_utils.h"

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
        std::map<std::string, BandStatistics> bands; // by "<band>|<effective NoData override>"
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

    namespace
    {
        // The key of an effective NoData value in the sidecar and in memory: "" for the NoData value of
        // the band
        std::string nodataKey(const std::optional<double> &nodata)
        {
            if (!nodata)
                return "";
            if (std::isnan(*nodata))
                return "nan";
            char text[40];
            std::snprintf(text, sizeof(text), "%.17g", *nodata);
            return text;
        }

        // The file whose pixels the statistics describe: the dataset when it is a file, else path
        std::string statsFileOf(const std::string &path, const RasterSource &source)
        {
            const std::string file = source.file();
            std::error_code ec;
            if (!file.empty() && fs::exists(io::fromUtf8(file), ec))
                return file;
            return path;
        }

        json statsToJson(const BandStatistics &s)
        {
            return {{"min", s.min}, {"max", s.max}, {"mean", s.mean}, {"std", s.stdDev},
                    {"valid", s.validCount}, {"total", s.pixelCount}};
        }

        // The usable sidecar of the raster: it exists, is not older than the raster, parses and says
        // that its statistics are exact. Null json otherwise.
        json readFreshSidecar(const fs::path &rasterPath)
        {
            const fs::path sidecar = statsSidecarOf(rasterPath);
            std::error_code ec;
            const auto sidecarTime = fs::last_write_time(sidecar, ec);
            if (ec)
                return nullptr;
            const auto rasterTime = fs::last_write_time(rasterPath, ec);
            // The COG build writes the sidecar after the raster: an older one describes
            // another raster that was at this path
            if (ec || sidecarTime < rasterTime)
                return nullptr;

            try
            {
                std::ifstream in(sidecar);
                if (!in.is_open())
                    return nullptr;
                json j = json::parse(in);
                // A sidecar of a build of before version 2 holds approximate statistics (the overviews
                // or a sample of the blocks) and does not say so: it is ignored
                if (!j.is_object() || j.value("exact", false) != true)
                    return nullptr;
                return j;
            }
            catch (const std::exception &e)
            {
                LOGW << "Ignoring unreadable statistics sidecar " << io::toUtf8(sidecar) << ": " << e.what();
                return nullptr;
            }
        }
    }

    namespace
    {
        // The pixels of a band that have a value (not NoData, not masked, finite), accumulated as they are read
        class BandAccumulator
        {
        public:
            BandAccumulator(const RasterSource &source, int band, const std::optional<double> &nodata)
                : nd_(nodata ? BandNodata{true, *nodata} : source.nodata(band)),
                  floatSamples_(source.isFloat32(band)), masked_(source.hasMask(band))
            {
            }

            bool masked() const { return masked_; }

            void add(const double *samples, const uint8_t *mask, size_t count)
            {
                for (size_t i = 0; i < count; i++)
                {
                    const double v = samples[i];
                    // A Float32 band holds its NoData value rounded to float (GDAL's rule, and DemContext::readRaw's):
                    // 1e-7 matches the float 1e-7 of the band
                    const bool isNd = floatSamples_ ? isNodata(static_cast<float>(v), nd_) : isNodata(v, nd_);
                    if (!std::isfinite(v) || isNd || (mask != nullptr && mask[i] == 0))
                        continue;
                    n_++;
                    lo_ = std::min(lo_, v);
                    hi_ = std::max(hi_, v);
                    const double delta = v - mean_;
                    mean_ += delta / static_cast<double>(n_);
                    m2_ += delta * (v - mean_);
                }
            }

            BandStatistics finish(uint64_t pixelCount, bool sampled) const
            {
                if (n_ == 0)
                    throw NoValidPixelsException(sampled ? "Cannot compute statistics: no pixel of the sample of the "
                                                           "band has a value"
                                                         : "Cannot compute statistics: no pixel of the band has a value",
                                                 sampled);
                BandStatistics s;
                s.min = lo_;
                s.max = hi_;
                s.mean = mean_;
                s.stdDev = std::sqrt(m2_ / static_cast<double>(n_));
                s.validCount = n_;
                s.pixelCount = pixelCount;
                s.sampled = sampled;
                return s;
            }

        private:
            BandNodata nd_;
            bool floatSamples_;
            bool masked_;
            double mean_ = 0.0, m2_ = 0.0;
            double lo_ = std::numeric_limits<double>::infinity();
            double hi_ = -std::numeric_limits<double>::infinity();
            uint64_t n_ = 0;
        };

        // About four million samples per read: the pass stops when the call is canceled
        constexpr size_t SamplesPerRead = size_t{1} << 22;

        // How much larger the second sample of the stretch is when the first has no pixel with a value
        constexpr uint64_t SparseSampleFactor = 16;

        // Bands read together, at a level of the source (0: the raster, i: overview i - 1), in one read
        struct BandSet
        {
            const RasterSource *source = nullptr;
            std::vector<int> bands;
            int level = 0;
            int width = 0;
            int height = 0;
        };

        // The bands of the source that `bands` names, at full resolution
        BandSet bandSetOf(const RasterSource &source, const std::vector<int> &bands)
        {
            for (const int b : bands)
                if (b < 1 || b > source.bandCount())
                    throw GDALException("Cannot read band " + std::to_string(b) + " to compute its statistics");
            return BandSet{&source, bands, 0, source.width(), source.height()};
        }

        // Overview `index` of every band of `full`, or nothing when a band lacks it or has it in another size
        std::optional<BandSet> overviewSetOf(const BandSet &full, int index)
        {
            const auto size = full.source->overviewSize(full.bands, index);
            if (!size)
                return std::nullopt;
            return BandSet{full.source, full.bands, index + 1, size->first, size->second};
        }

        // Reads the window of the bands of `set` into their accumulators
        void accumulateWindow(const BandSet &set, std::vector<BandAccumulator> &accumulators, int x, int y, int w,
                              int h, std::vector<double> &samples, std::vector<uint8_t> &mask)
        {
            const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
            const size_t bandCount = set.bands.size();
            samples.resize(count * bandCount);
            const bool masked = std::any_of(accumulators.begin(), accumulators.end(),
                                            [](const BandAccumulator &a) { return a.masked(); });
            if (masked)
                mask.resize(count);
            set.source->read(set.level, RasterWindow{x, y, w, h, w, h}, set.bands, samples.data(),
                             masked ? mask.data() : nullptr);
            for (size_t i = 0; i < bandCount; i++)
                accumulators[i].add(samples.data() + i * count, accumulators[i].masked() ? mask.data() : nullptr,
                                    count);
        }

        std::vector<BandAccumulator> accumulatorsOf(const BandSet &set, const std::optional<double> &nodata)
        {
            std::vector<BandAccumulator> accumulators;
            accumulators.reserve(set.bands.size());
            for (const int b : set.bands)
                accumulators.emplace_back(*set.source, b, nodata);
            return accumulators;
        }

        // Every pixel of the bands of `set`, rows at a time (the pass stops when the call is canceled). `progress`,
        // when set, is told the fraction of the rows read after each read
        void accumulateAll(const BandSet &set, std::vector<BandAccumulator> &accumulators,
                           const std::function<void(double)> &progress = {})
        {
            const size_t rowSamples = std::max<size_t>(1, static_cast<size_t>(set.width) * set.bands.size());
            const int rowsPerRead = std::max(1, static_cast<int>(SamplesPerRead / rowSamples));
            std::vector<double> samples;
            std::vector<uint8_t> mask;
            for (int y = 0; y < set.height; y += rowsPerRead)
            {
                cancel::throwIfCanceled();
                const int rows = std::min(rowsPerRead, set.height - y);
                accumulateWindow(set, accumulators, 0, y, set.width, rows, samples, mask);
                if (progress)
                    progress(static_cast<double>(y + rows) / static_cast<double>(set.height));
            }
        }

        std::vector<BandStatistics> finishAll(const std::vector<BandAccumulator> &accumulators, uint64_t pixelCount,
                                              bool sampled)
        {
            std::vector<BandStatistics> result;
            result.reserve(accumulators.size());
            for (const auto &acc : accumulators)
                result.push_back(acc.finish(pixelCount, sampled));
            return result;
        }

        // The step of an even lattice over a grid of width x height cells that keeps at most maxCells of
        // them: 1 when the grid has no more cells, else the smallest step whose lattice
        // (ceil(width / step) x ceil(height / step)) has at most maxCells cells
        uint64_t latticeStep(uint64_t width, uint64_t height, uint64_t maxCells)
        {
            width = std::max<uint64_t>(1, width);
            height = std::max<uint64_t>(1, height);
            maxCells = std::max<uint64_t>(1, maxCells);
            if (width * height <= maxCells)
                return 1;
            const auto cells = [&](uint64_t s) { return ((width + s - 1) / s) * ((height + s - 1) / s); };
            uint64_t step = std::max<uint64_t>(
                2, static_cast<uint64_t>(std::ceil(std::sqrt(static_cast<double>(width * height) / static_cast<double>(maxCells)))));
            // A long and narrow grid needs more than the square root (a 1 x 1 lattice always fits)
            while (cells(step) > maxCells)
                step++;
            return step;
        }
    }

    std::vector<BandStatistics> computeBandStatistics(const RasterSource &source, const std::vector<int> &bands,
                                                      const std::optional<double> &nodata)
    {
        if (bands.empty())
            return {};
        const BandSet set = bandSetOf(source, bands);
        auto accumulators = accumulatorsOf(set, nodata);
        accumulateAll(set, accumulators);
        return finishAll(accumulators, static_cast<uint64_t>(set.width) * static_cast<uint64_t>(set.height), false);
    }

    namespace
    {
        // Calls `read` with the windows of a grid of cells of cellWidth x cellHeight pixels from the corner of the
        // region (x, y, width, height), cut at its right and bottom edges, row by row; each window is read at its size
        void forEachGridWindow(int x, int y, int width, int height, int cellWidth, int cellHeight,
                               const std::function<void(const RasterWindow &)> &read)
        {
            cellWidth = std::max(1, cellWidth);
            cellHeight = std::max(1, cellHeight);
            for (int y0 = 0; y0 < height; y0 += cellHeight)
            {
                const int h = std::min(cellHeight, height - y0);
                for (int x0 = 0; x0 < width; x0 += cellWidth)
                {
                    const int w = std::min(cellWidth, width - x0);
                    read(RasterWindow{x + x0, y + y0, w, h, w, h});
                }
            }
        }

        // The pixel of `size` source pixels that output pixel `i` of `out` starts at
        int scaledEdge(int i, int out, int size)
        {
            return static_cast<int>(static_cast<int64_t>(i) * size / out);
        }
    }

    void BoundedSample::forEachBlock(const std::function<void(const RasterWindow &)> &read) const
    {
        if (kind != Kind::Blocks)
            return;
        // The middle block of each step x step cell of blocks
        const uint64_t offset = step / 2;
        const int64_t partRight = static_cast<int64_t>(part.x) + part.xsize;
        const int64_t partBottom = static_cast<int64_t>(part.y) + part.ysize;
        for (uint64_t cy = 0; cy * step < blocksY; cy++)
        {
            const uint64_t by = firstBlockY + std::min(cy * step + offset, blocksY - 1);
            const int64_t top = static_cast<int64_t>(by) * blockHeight;
            const int y0 = static_cast<int>(std::max<int64_t>(top, part.y));
            const int y1 = static_cast<int>(std::min<int64_t>(top + blockHeight, partBottom));
            for (uint64_t cx = 0; cx * step < blocksX; cx++)
            {
                cancel::throwIfCanceled();
                const uint64_t bx = firstBlockX + std::min(cx * step + offset, blocksX - 1);
                const int64_t left = static_cast<int64_t>(bx) * blockWidth;
                const int x0 = static_cast<int>(std::max<int64_t>(left, part.x));
                const int x1 = static_cast<int>(std::min<int64_t>(left + blockWidth, partRight));
                read(RasterWindow{x0, y0, x1 - x0, y1 - y0, x1 - x0, y1 - y0});
            }
        }
    }

    void BoundedSample::forEachWindow(int maxSide, const std::function<void(const RasterWindow &)> &read) const
    {
        maxSide = std::max(1, maxSide);
        switch (kind)
        {
        case Kind::Whole:
            forEachGridWindow(part.x, part.y, part.xsize, part.ysize, maxSide, maxSide, read);
            return;
        case Kind::Blocks:
            forEachBlock([&](const RasterWindow &block) {
                forEachGridWindow(block.x, block.y, block.xsize, block.ysize, maxSide, maxSide, read);
            });
            return;
        case Kind::Overview:
        case Kind::Decimated:
        {
            // A grid over the window at the size it is read at, whose cells cover at most maxSide pixels of the
            // raster across and down, each read from the pixels of the raster it covers
            const int cellWidth = static_cast<int>(static_cast<int64_t>(maxSide) * outWidth / part.xsize);
            const int cellHeight = static_cast<int>(static_cast<int64_t>(maxSide) * outHeight / part.ysize);
            forEachGridWindow(0, 0, outWidth, outHeight, cellWidth, cellHeight, [&](const RasterWindow &out) {
                cancel::throwIfCanceled();
                const int x0 = part.x + scaledEdge(out.x, outWidth, part.xsize);
                const int x1 = part.x + scaledEdge(out.x + out.xsize, outWidth, part.xsize);
                const int y0 = part.y + scaledEdge(out.y, outHeight, part.ysize);
                const int y1 = part.y + scaledEdge(out.y + out.ysize, outHeight, part.ysize);
                read(RasterWindow{x0, y0, x1 - x0, y1 - y0, out.xsize, out.ysize});
            });
            return;
        }
        }
    }

    BoundedSample planBoundedSample(const RasterSource &source, const std::vector<int> &bands,
                                    const RasterWindow &part, uint64_t maxPixels, BoundedSample::Fallback fallback)
    {
        maxPixels = std::max<uint64_t>(1, maxPixels);
        const BandSet full = bandSetOf(source, bands);
        BoundedSample sample;
        sample.part = part;
        const uint64_t partWidth = static_cast<uint64_t>(std::max(0, part.xsize));
        const uint64_t partHeight = static_cast<uint64_t>(std::max(0, part.ysize));
        if (full.bands.empty() || partWidth * partHeight <= maxPixels)
            return sample; // small enough to be read whole

        // The largest overview in which the window is within the budget
        const int overviewCount = source.overviewCount();
        uint64_t overviewPixels = 0;
        for (int i = 0; i < overviewCount; i++)
        {
            const auto candidate = overviewSetOf(full, i);
            if (!candidate || candidate->width <= 0 || candidate->height <= 0)
                continue;
            // The window in the pixels of the overview, rounded up
            const uint64_t w = std::max<uint64_t>(
                1, (partWidth * candidate->width + full.width - 1) / static_cast<uint64_t>(full.width));
            const uint64_t h = std::max<uint64_t>(
                1, (partHeight * candidate->height + full.height - 1) / static_cast<uint64_t>(full.height));
            if (w * h <= maxPixels && w * h > overviewPixels && w <= partWidth && h <= partHeight)
            {
                sample.kind = BoundedSample::Kind::Overview;
                sample.overview = i;
                sample.outWidth = static_cast<int>(w);
                sample.outHeight = static_cast<int>(h);
                overviewPixels = w * h;
            }
        }
        if (sample.kind == BoundedSample::Kind::Overview)
            return sample;

        if (fallback == BoundedSample::Fallback::Decimated)
        {
            // The largest size of the proportions of the window within the budget
            const double scale =
                std::sqrt(static_cast<double>(maxPixels) / static_cast<double>(partWidth * partHeight));
            const auto scaled = [scale](uint64_t size) {
                return std::clamp<uint64_t>(static_cast<uint64_t>(static_cast<double>(size) * scale), 1, size);
            };
            uint64_t w = scaled(partWidth);
            uint64_t h = scaled(partHeight);
            while (w * h > maxPixels)
            {
                if (w >= h && w > 1)
                    w--;
                else
                    h--;
            }
            sample.kind = BoundedSample::Kind::Decimated;
            sample.outWidth = static_cast<int>(w);
            sample.outHeight = static_cast<int>(h);
            return sample;
        }

        // Else whole blocks of the raster on an even lattice, at most maxPixels pixels: only the blocks
        // read are decoded (a decimated read of a raster without overviews decodes every block, and
        // some of them several times)
        int blockWidth = 0, blockHeight = 0;
        source.blockSize(blockWidth, blockHeight);
        sample.kind = BoundedSample::Kind::Blocks;
        sample.blockWidth = std::clamp(blockWidth, 1, std::max(1, full.width));
        sample.blockHeight = std::clamp(blockHeight, 1, std::max(1, full.height));
        sample.firstBlockX = static_cast<uint64_t>(part.x) / sample.blockWidth;
        sample.firstBlockY = static_cast<uint64_t>(part.y) / sample.blockHeight;
        sample.blocksX = (static_cast<uint64_t>(part.x) + partWidth - 1) / sample.blockWidth - sample.firstBlockX + 1;
        sample.blocksY = (static_cast<uint64_t>(part.y) + partHeight - 1) / sample.blockHeight - sample.firstBlockY + 1;
        const uint64_t maxBlocks = std::max<uint64_t>(
            1, maxPixels / (static_cast<uint64_t>(sample.blockWidth) * static_cast<uint64_t>(sample.blockHeight)));
        sample.step = latticeStep(sample.blocksX, sample.blocksY, maxBlocks);
        return sample;
    }

    uint64_t sparseSampleBudget(uint64_t maxPixels)
    {
        return maxPixels > std::numeric_limits<uint64_t>::max() / SparseSampleFactor
                   ? std::numeric_limits<uint64_t>::max()
                   : maxPixels * SparseSampleFactor;
    }

    std::vector<BandStatistics> sampleBandStatistics(const RasterSource &source, const std::vector<int> &bands,
                                                     const std::optional<double> &nodata, uint64_t maxPixels)
    {
        if (bands.empty())
            return {};
        const BandSet full = bandSetOf(source, bands);
        const uint64_t pixelCount = static_cast<uint64_t>(full.width) * static_cast<uint64_t>(full.height);
        const BoundedSample sample =
            planBoundedSample(source, bands, RasterWindow{0, 0, full.width, full.height, full.width, full.height},
                              maxPixels);
        switch (sample.kind)
        {
        case BoundedSample::Kind::Whole:
        {
            // Small enough to be read whole: the exact statistics
            auto accumulators = accumulatorsOf(full, nodata);
            accumulateAll(full, accumulators);
            return finishAll(accumulators, pixelCount, false);
        }
        case BoundedSample::Kind::Overview:
        {
            // The overview, read whole
            const BandSet overview = *overviewSetOf(full, sample.overview);
            auto accumulators = accumulatorsOf(overview, nodata);
            accumulateAll(overview, accumulators);
            return finishAll(accumulators, pixelCount, true);
        }
        case BoundedSample::Kind::Blocks:
        case BoundedSample::Kind::Decimated: // not planned with the fallback of whole blocks
            break;
        }

        auto accumulators = accumulatorsOf(full, nodata);
        std::vector<double> samples;
        std::vector<uint8_t> mask;
        uint64_t read = 0;
        sample.forEachBlock([&](const RasterWindow &block) {
            accumulateWindow(full, accumulators, block.x, block.y, block.xsize, block.ysize, samples, mask);
            read++;
        });
        LOGD << "Sampled " << read << " of " << sample.blocksX * sample.blocksY << " blocks (" << sample.blockWidth
             << "x" << sample.blockHeight << ") for the statistics of the stretch";
        return finishAll(accumulators, pixelCount, true);
    }

    BandStatistics RasterStatsProvider::get(const std::string &path, const RasterSource &source, int band,
                                            const std::optional<double> &nodata, bool persist)
    {
        const auto entry = entryFor(path);
        std::lock_guard<std::mutex> lock(entry->mutex);

        const std::string key = std::to_string(band) + "|" + nodataKey(nodata);
        const auto cached = entry->bands.find(key);
        if (cached != entry->bands.end())
            return cached->second;

        if (band < 1 || band > source.bandCount())
            throw GDALException("Cannot read band " + std::to_string(band) + " of " + path);

        const std::string file = statsFileOf(path, source);
        BandStatistics s;
        Source from;
        if (readStatsSidecar(file, band, s, nodata))
        {
            from = Source::Sidecar;
        }
        else
        {
            s = computeBandStatistics(source, {band}, nodata).front();
            from = Source::Computed;
            LOGD << "Computed band " << band << " statistics of " << path << " (" << s.min << ", "
                 << s.max << ")";
            if (persist || raster::isLibddbArtefact(io::fromUtf8(file)))
                writeStatsSidecar(file, band, s, nodata);
        }

        entry->bands[key] = s;

        std::lock_guard<std::mutex> countersLock(mutex_);
        switch (from)
        {
        case Source::Sidecar:
            counters_.fromSidecar++;
            break;
        case Source::Computed:
            counters_.computed++;
            break;
        }
        return s;
    }

    std::vector<std::optional<BandStatistics>> RasterStatsProvider::getAll(const std::string &path,
                                                                          const RasterSource &source,
                                                                          const std::vector<int> &bands,
                                                                          const std::optional<double> &nodata,
                                                                          bool persist,
                                                                          const std::function<void(double)> &progress)
    {
        const auto entry = entryFor(path);
        std::lock_guard<std::mutex> lock(entry->mutex);

        const std::string file = statsFileOf(path, source);
        const auto keyOf = [&nodata](int band) { return std::to_string(band) + "|" + nodataKey(nodata); };

        std::vector<std::optional<BandStatistics>> result(bands.size());
        std::vector<size_t> missingAt; // the positions in `bands` of the bands that are not known
        std::vector<int> missing;
        uint64_t fromSidecar = 0;
        for (size_t i = 0; i < bands.size(); i++)
        {
            const int b = bands[i];
            if (b < 1 || b > source.bandCount())
                throw GDALException("Cannot read band " + std::to_string(b) + " of " + path);

            const auto cached = entry->bands.find(keyOf(b));
            if (cached != entry->bands.end())
            {
                result[i] = cached->second;
                continue;
            }
            BandStatistics s;
            if (readStatsSidecar(file, b, s, nodata))
            {
                entry->bands[keyOf(b)] = s;
                result[i] = s;
                fromSidecar++;
                continue;
            }
            missingAt.push_back(i);
            missing.push_back(b);
        }

        uint64_t computed = 0;
        if (!missing.empty())
        {
            // The pass of computeBandStatistics, but each band finished on its own: a band without a pixel
            // that has a value does not lose the statistics of the others
            const BandSet set = bandSetOf(source, missing);
            auto accumulators = accumulatorsOf(set, nodata);
            accumulateAll(set, accumulators, progress);
            const uint64_t pixelCount = static_cast<uint64_t>(set.width) * static_cast<uint64_t>(set.height);
            for (size_t j = 0; j < missing.size(); j++)
            {
                const int b = missing[j];
                BandStatistics s;
                try
                {
                    s = accumulators[j].finish(pixelCount, false);
                }
                catch (const NoValidPixelsException &)
                {
                    continue; // nullopt, where get() throws
                }
                entry->bands[keyOf(b)] = s;
                result[missingAt[j]] = s;
                computed++;
                LOGD << "Computed band " << b << " statistics of " << path << " (" << s.min << ", " << s.max << ")";
                if (persist || raster::isLibddbArtefact(io::fromUtf8(file)))
                    writeStatsSidecar(file, b, s, nodata);
            }
        }
        else if (progress)
        {
            progress(1.0);
        }

        std::lock_guard<std::mutex> countersLock(mutex_);
        counters_.fromSidecar += fromSidecar;
        counters_.computed += computed;
        return result;
    }

    std::vector<BandStatistics> RasterStatsProvider::getStretchStatistics(const std::string &path,
                                                                          const RasterSource &source,
                                                                          const std::vector<int> &bands,
                                                                          const std::optional<double> &nodata,
                                                                          uint64_t maxPixels)
    {
        const auto entry = entryFor(path);
        std::lock_guard<std::mutex> lock(entry->mutex);

        const std::string file = statsFileOf(path, source);
        const auto keyOf = [&nodata](int band) { return std::to_string(band) + "|" + nodataKey(nodata); };
        // A sample is kept under its own key: get() (value info, contours) never returns it
        const auto sampledKeyOf = [&keyOf](int band) { return keyOf(band) + "|sampled"; };

        std::map<int, BandStatistics> known;
        std::vector<int> missing;
        uint64_t fromSidecar = 0;
        for (const int b : bands)
        {
            if (known.count(b) != 0 || std::find(missing.begin(), missing.end(), b) != missing.end())
                continue;
            if (b < 1 || b > source.bandCount())
                throw GDALException("Cannot read band " + std::to_string(b) + " of " + path);

            // The exact statistics when they are known, else a sample read before
            const auto exact = entry->bands.find(keyOf(b));
            if (exact != entry->bands.end())
            {
                known[b] = exact->second;
                continue;
            }
            BandStatistics s;
            if (readStatsSidecar(file, b, s, nodata))
            {
                entry->bands[keyOf(b)] = s;
                known[b] = s;
                fromSidecar++;
                continue;
            }
            const auto sample = entry->bands.find(sampledKeyOf(b));
            if (sample != entry->bands.end())
            {
                known[b] = sample->second;
                continue;
            }
            missing.push_back(b);
        }

        uint64_t computed = 0, sampled = 0;
        if (!missing.empty())
        {
            std::vector<BandStatistics> stats;
            try
            {
                stats = sampleBandStatistics(source, missing, nodata, maxPixels);
            }
            catch (const GDALException &)
            {
                cancel::throwIfCanceled();
                // No pixel of the sample has a value (a sparse raster): once more with a larger budget,
                // still bounded (the exact statistics when the band fits in it)
                LOGD << "No pixel of the sample of the bands of " << path << " has a value: a larger sample";
                stats = sampleBandStatistics(source, missing, nodata, sparseSampleBudget(maxPixels));
            }
            for (size_t i = 0; i < missing.size(); i++)
            {
                const int b = missing[i];
                const BandStatistics &s = stats[i];
                known[b] = s;
                if (s.sampled)
                {
                    entry->bands[sampledKeyOf(b)] = s;
                    sampled++;
                    LOGD << "Sampled band " << b << " statistics of " << path << " (" << s.validCount
                         << " pixels with a value): (" << s.min << ", " << s.max << ")";
                }
                else
                {
                    entry->bands[keyOf(b)] = s;
                    computed++;
                    LOGD << "Computed band " << b << " statistics of " << path << " (" << s.min << ", "
                         << s.max << ")";
                    if (raster::isLibddbArtefact(io::fromUtf8(file)))
                        writeStatsSidecar(file, b, s, nodata);
                }
            }
        }

        {
            std::lock_guard<std::mutex> countersLock(mutex_);
            counters_.fromSidecar += fromSidecar;
            counters_.computed += computed;
            counters_.sampled += sampled;
        }

        std::vector<BandStatistics> result;
        result.reserve(bands.size());
        for (const int b : bands)
            result.push_back(known.at(b));
        return result;
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

    std::pair<double, double> approximateMinMax(const RasterSource &source, int band)
    {
        const auto statsMin = source.metadataItem(band, "STATISTICS_MINIMUM");
        const auto statsMax = source.metadataItem(band, "STATISTICS_MAXIMUM");
        if (statsMin && statsMax)
            return {CPLAtofM(statsMin->c_str()), CPLAtofM(statsMax->c_str())};

        const GDALDataType type = source.dataType(band);
        const BandNodata nodata = source.nodata(band);
        const bool masked = !nodata.has && source.hasMask(band) && !source.isAlpha(band);
        // GDALNoDataValues: a Float32 band compares its samples with the NoData value as floats
        const bool floatNodata = type == GDT_Float32 && nodata.has && !std::isnan(nodata.value) &&
                                 std::fabs(nodata.value) <= std::numeric_limits<float>::max();
        const auto isValue = [&](double v) {
            if (std::isnan(v))
                return false;
            if (!nodata.has)
                return true;
            if (floatNodata)
                return static_cast<float>(v) != static_cast<float>(nodata.value);
            return v != nodata.value;
        };
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        std::vector<double> samples;
        std::vector<uint8_t> mask;
        const auto accumulate = [&](int level, const RasterWindow &w) {
            cancel::throwIfCanceled();
            const size_t count = w.outPixels();
            samples.resize(count);
            if (masked)
                mask.resize(count);
            source.read(level, w, {band}, samples.data(), masked ? mask.data() : nullptr);
            for (size_t i = 0; i < count; i++)
            {
                if (masked && mask[i] == 0)
                    continue;
                if (!isValue(samples[i]))
                    continue;
                lo = std::min(lo, samples[i]);
                hi = std::max(hi, samples[i]);
            }
        };

        // GetRasterSampleOverview(GDALSTAT_APPROX_NUMSAMPLES)
        constexpr double kApproxSamples = 2500;
        int level = 0;
        double best = static_cast<double>(source.width()) * source.height();
        for (int i = 0; i < source.overviewCount(); i++)
        {
            const auto size = source.overviewSize({band}, i);
            if (!size)
                continue;
            const double pixels = static_cast<double>(size->first) * size->second;
            if (pixels < best && pixels > kApproxSamples)
            {
                best = pixels;
                level = i + 1;
            }
        }
        if (level > 0)
        {
            // Every pixel of the overview, rows at a time
            const auto [ow, oh] = *source.overviewSize({band}, level - 1);
            const int rows = std::max(1, static_cast<int>((size_t{1} << 22) / static_cast<size_t>(std::max(1, ow))));
            for (int y = 0; y < oh; y += rows)
            {
                const int n = std::min(rows, oh - y);
                accumulate(level, RasterWindow{0, y, ow, n, ow, n});
            }
            if (lo > hi)
            {
                // ComputeRasterMinMaxLocation (32 and 64-bit samples without a mask) leaves its initial range
                const bool location = !masked && type != GDT_Byte && type != GDT_UInt16 && type != GDT_Int16;
                return location ? std::pair<double, double>{std::numeric_limits<double>::infinity(),
                                                            -std::numeric_limits<double>::infinity()}
                                : std::pair<double, double>{0.0, 0.0};
            }
            return {lo, hi};
        }

        // Every n-th block of the full resolution
        int bw = 0, bh = 0;
        source.blockSize(bw, bh);
        bw = std::max(1, bw);
        bh = std::max(1, bh);
        const int64_t across = (source.width() + bw - 1) / bw, down = (source.height() + bh - 1) / bh;
        int64_t rate = std::max<int64_t>(1, static_cast<int64_t>(std::sqrt(static_cast<double>(across) * static_cast<double>(down))));
        if (rate == across && across > 1)
            rate++;
        for (int64_t block = 0; block < across * down; block += rate)
        {
            const int bx = static_cast<int>(block % across), by = static_cast<int>(block / across);
            const int x = bx * bw, y = by * bh;
            const int w = std::min(bw, source.width() - x), h = std::min(bh, source.height() - y);
            accumulate(0, RasterWindow{x, y, w, h, w, h});
        }
        if (lo > hi)
            return {0.0, 0.0};
        return {lo, hi};
    }

    fs::path statsSidecarOf(const fs::path &raster)
    {
        return io::concat(raster, ".stats.json");
    }

    bool readStatsSidecar(const std::string &path, int band, BandStatistics &out, const std::optional<double> &nodata)
    {
        const json j = readFreshSidecar(io::fromUtf8(path));
        if (j.is_null())
            return false;

        try
        {
            const json *bands = nullptr;
            if (!nodata)
            {
                const auto it = j.find("bands");
                if (it != j.end())
                    bands = &*it;
            }
            else
            {
                const auto overrides = j.find("overrides");
                if (overrides != j.end())
                {
                    const auto forKey = overrides->find(nodataKey(nodata));
                    if (forKey != overrides->end())
                        bands = &*forKey;
                }
            }
            if (bands == nullptr)
                return false;
            const auto b = bands->find(std::to_string(band));
            if (b == bands->end())
                return false;

            out.min = b->at("min").get<double>();
            out.max = b->at("max").get<double>();
            out.mean = b->at("mean").get<double>();
            out.stdDev = b->at("std").get<double>();
            out.validCount = b->value("valid", static_cast<uint64_t>(0));
            out.pixelCount = b->value("total", static_cast<uint64_t>(0));
            return true;
        }
        catch (const std::exception &e)
        {
            LOGW << "Ignoring unreadable statistics sidecar of " << path << ": " << e.what();
            return false;
        }
    }

    bool writeStatsSidecar(const std::string &path, int band, const BandStatistics &stats,
                           const std::optional<double> &nodata)
    {
        const fs::path rasterPath = io::fromUtf8(path);
        try
        {
            json j = readFreshSidecar(rasterPath);
            if (j.is_null())
                j = json::object();

            if (!nodata)
                j["bands"][std::to_string(band)] = statsToJson(stats);
            else
                j["overrides"][nodataKey(nodata)][std::to_string(band)] = statsToJson(stats);
            j["version"] = 2;
            j["exact"] = true;

            const auto now = std::chrono::system_clock::now();
            const auto time = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#ifdef _WIN32
            gmtime_s(&tm, &time);
#else
            gmtime_r(&time, &tm);
#endif
            std::ostringstream stamp;
            stamp << std::put_time(&tm, "%FT%TZ");
            j["computedAt"] = stamp.str();

            // Whole, by a rename: a reader never sees half of it
            const fs::path target = statsSidecarOf(rasterPath);
            const fs::path temp = io::concat(target, "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".tmp");
            {
                std::ofstream out(temp, std::ios::trunc);
                if (!out.is_open())
                    throw AppException("Cannot write " + io::toUtf8(temp));
                out << j.dump(2);
                out.flush();
                if (!out)
                    throw AppException("Cannot write " + io::toUtf8(temp));
            }
            std::error_code ec;
            fs::rename(temp, target, ec);
            if (ec)
            {
                fs::remove(temp, ec);
                throw AppException("Cannot replace " + io::toUtf8(target));
            }
            return true;
        }
        catch (const std::exception &e)
        {
            LOGW << "Cannot write the statistics sidecar of " << path << ": " << e.what();
            return false;
        }
    }
}
