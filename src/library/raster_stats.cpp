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
        std::string statsFileOf(const std::string &path, GDALDatasetH ds)
        {
            const char *description = ds != nullptr ? GDALGetDescription(ds) : nullptr;
            std::error_code ec;
            if (description != nullptr && *description != '\0' && fs::exists(io::fromUtf8(description), ec))
                return description;
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
            BandAccumulator(GDALRasterBandH hBand, const std::optional<double> &nodata)
                : nd_(readBandNodata(hBand)), floatSamples_(GDALGetRasterDataType(hBand) == GDT_Float32)
            {
                if (nodata)
                    nd_ = BandNodata{true, *nodata};

                // The transparency that is not the NoData value: an alpha band or a mask of the dataset
                const int maskFlags = GDALGetMaskFlags(hBand);
                if ((maskFlags & (GMF_ALPHA | GMF_PER_DATASET)) != 0 && (maskFlags & (GMF_NODATA | GMF_ALL_VALID)) == 0)
                    maskBand_ = GDALGetMaskBand(hBand);
            }

            GDALRasterBandH maskBand() const { return maskBand_; }

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
            GDALRasterBandH maskBand_ = nullptr;
            double mean_ = 0.0, m2_ = 0.0;
            double lo_ = std::numeric_limits<double>::infinity();
            double hi_ = -std::numeric_limits<double>::infinity();
            uint64_t n_ = 0;
        };

        // About four million samples per read: the pass stops when the call is canceled
        constexpr size_t SamplesPerRead = size_t{1} << 22;

        // How much larger the second sample of the stretch is when the first has no pixel with a value
        constexpr uint64_t SparseSampleFactor = 16;

        // Bands read together: in one read of `ds` when it holds them all (a file whose blocks hold every
        // band, pixel interleaving, is then decoded once and not once per band), else band by band
        struct BandSet
        {
            GDALDatasetH ds = nullptr;
            std::vector<int> numbers;            // in ds
            std::vector<GDALRasterBandH> bands;  // the same bands
            int width = 0;
            int height = 0;
        };

        // The bands of ds that `bands` names
        BandSet bandSetOf(GDALDatasetH ds, const std::vector<int> &bands)
        {
            BandSet set;
            set.ds = ds;
            set.numbers = bands;
            set.width = GDALGetRasterXSize(ds);
            set.height = GDALGetRasterYSize(ds);
            for (const int b : bands)
            {
                const GDALRasterBandH hBand = GDALGetRasterBand(ds, b);
                if (hBand == nullptr)
                    throw GDALException("Cannot read band " + std::to_string(b) + " to compute its statistics");
                set.bands.push_back(hBand);
            }
            return set;
        }

        // Overview `index` of every band of `full`, or nothing when a band lacks it or has it in another size
        std::optional<BandSet> overviewSetOf(const BandSet &full, int index)
        {
            BandSet set;
            for (const GDALRasterBandH hBand : full.bands)
            {
                const GDALRasterBandH ov = GDALGetOverview(hBand, index);
                if (ov == nullptr)
                    return std::nullopt;
                if (set.bands.empty())
                {
                    set.width = GDALGetRasterBandXSize(ov);
                    set.height = GDALGetRasterBandYSize(ov);
                }
                else if (GDALGetRasterBandXSize(ov) != set.width || GDALGetRasterBandYSize(ov) != set.height)
                {
                    return std::nullopt;
                }
                set.bands.push_back(ov);
            }
            // One read when the overviews are the bands of one dataset (the overview level of a GeoTIFF)
            GDALDatasetH ovDs = GDALGetBandDataset(set.bands.front());
            for (size_t i = 0; ovDs != nullptr && i < set.bands.size(); i++)
            {
                const int number = GDALGetBandNumber(set.bands[i]);
                if (GDALGetBandDataset(set.bands[i]) != ovDs || number <= 0 ||
                    GDALGetRasterBand(ovDs, number) != set.bands[i])
                    ovDs = nullptr;
                else
                    set.numbers.push_back(number);
            }
            set.ds = ovDs;
            if (ovDs == nullptr)
                set.numbers.clear();
            return set;
        }

        // Reads the window of the bands of `set` into their accumulators
        void accumulateWindow(const BandSet &set, std::vector<BandAccumulator> &accumulators, int x, int y, int w,
                              int h, std::vector<double> &samples, std::vector<uint8_t> &mask)
        {
            const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
            const size_t bandCount = set.bands.size();
            samples.resize(count * bandCount);
            cancel::GdalProgress progress;
            GDALRasterIOExtraArg extra = progress.rasterIOExtraArg();
            if (set.ds != nullptr)
            {
                std::vector<int> numbers(set.numbers);
                if (GDALDatasetRasterIOEx(set.ds, GF_Read, x, y, w, h, samples.data(), w, h, GDT_Float64,
                                          static_cast<int>(bandCount), numbers.data(), 0, 0, 0, &extra) != CE_None)
                {
                    cancel::throwIfCanceled();
                    throw GDALException("Cannot read the bands to compute their statistics");
                }
            }
            else
            {
                for (size_t i = 0; i < bandCount; i++)
                {
                    GDALRasterIOExtraArg bandExtra = progress.rasterIOExtraArg();
                    if (GDALRasterIOEx(set.bands[i], GF_Read, x, y, w, h, samples.data() + i * count, w, h,
                                       GDT_Float64, 0, 0, &bandExtra) != CE_None)
                    {
                        cancel::throwIfCanceled();
                        throw GDALException("Cannot read a band to compute its statistics");
                    }
                }
            }

            GDALRasterBandH maskRead = nullptr; // the mask in `mask`: bands that share one read it once
            for (size_t i = 0; i < bandCount; i++)
            {
                const GDALRasterBandH maskBand = accumulators[i].maskBand();
                if (maskBand != nullptr && maskBand != maskRead)
                {
                    mask.resize(count);
                    GDALRasterIOExtraArg maskExtra = progress.rasterIOExtraArg();
                    if (GDALRasterIOEx(maskBand, GF_Read, x, y, w, h, mask.data(), w, h, GDT_Byte, 0, 0,
                                       &maskExtra) != CE_None)
                    {
                        cancel::throwIfCanceled();
                        throw GDALException("Cannot read the mask of a band to compute its statistics");
                    }
                    maskRead = maskBand;
                }
                accumulators[i].add(samples.data() + i * count, maskBand != nullptr ? mask.data() : nullptr, count);
            }
        }

        std::vector<BandAccumulator> accumulatorsOf(const BandSet &set, const std::optional<double> &nodata)
        {
            std::vector<BandAccumulator> accumulators;
            accumulators.reserve(set.bands.size());
            for (const GDALRasterBandH hBand : set.bands)
                accumulators.emplace_back(hBand, nodata);
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

    BandStatistics computeExactStatistics(GDALRasterBandH hBand, const std::optional<double> &nodata)
    {
        BandSet set;
        set.bands = {hBand};
        set.width = GDALGetRasterBandXSize(hBand);
        set.height = GDALGetRasterBandYSize(hBand);
        auto accumulators = accumulatorsOf(set, nodata);
        accumulateAll(set, accumulators);
        return accumulators.front().finish(static_cast<uint64_t>(set.width) * static_cast<uint64_t>(set.height), false);
    }

    std::vector<BandStatistics> computeBandStatistics(GDALDatasetH ds, const std::vector<int> &bands,
                                                      const std::optional<double> &nodata)
    {
        if (bands.empty())
            return {};
        const BandSet set = bandSetOf(ds, bands);
        auto accumulators = accumulatorsOf(set, nodata);
        accumulateAll(set, accumulators);
        return finishAll(accumulators, static_cast<uint64_t>(set.width) * static_cast<uint64_t>(set.height), false);
    }

    std::vector<BandStatistics> sampleBandStatistics(GDALDatasetH ds, const std::vector<int> &bands,
                                                     const std::optional<double> &nodata, uint64_t maxPixels)
    {
        if (bands.empty())
            return {};
        maxPixels = std::max<uint64_t>(1, maxPixels);
        const BandSet full = bandSetOf(ds, bands);
        const uint64_t pixelCount = static_cast<uint64_t>(full.width) * static_cast<uint64_t>(full.height);
        if (pixelCount <= maxPixels)
        {
            // Small enough to be read whole: the exact statistics
            auto accumulators = accumulatorsOf(full, nodata);
            accumulateAll(full, accumulators);
            return finishAll(accumulators, pixelCount, false);
        }

        // The largest overview that is within the budget, read whole
        std::optional<BandSet> overview;
        uint64_t overviewPixels = 0;
        const int overviewCount = GDALGetOverviewCount(full.bands.front());
        for (int i = 0; i < overviewCount; i++)
        {
            auto candidate = overviewSetOf(full, i);
            if (!candidate)
                continue;
            const uint64_t pixels = static_cast<uint64_t>(candidate->width) * static_cast<uint64_t>(candidate->height);
            if (pixels > 0 && pixels <= maxPixels && pixels > overviewPixels)
            {
                overview = std::move(candidate);
                overviewPixels = pixels;
            }
        }
        if (overview)
        {
            auto accumulators = accumulatorsOf(*overview, nodata);
            accumulateAll(*overview, accumulators);
            return finishAll(accumulators, pixelCount, true);
        }

        // Else whole blocks of the raster on an even lattice, at most maxPixels pixels: only the blocks
        // read are decoded (a decimated read of a raster without overviews decodes every block, and
        // some of them several times)
        int blockWidth = 0, blockHeight = 0;
        GDALGetBlockSize(full.bands.front(), &blockWidth, &blockHeight);
        blockWidth = std::clamp(blockWidth, 1, full.width);
        blockHeight = std::clamp(blockHeight, 1, full.height);
        const uint64_t blocksX = (static_cast<uint64_t>(full.width) + blockWidth - 1) / blockWidth;
        const uint64_t blocksY = (static_cast<uint64_t>(full.height) + blockHeight - 1) / blockHeight;
        const uint64_t maxBlocks =
            std::max<uint64_t>(1, maxPixels / (static_cast<uint64_t>(blockWidth) * static_cast<uint64_t>(blockHeight)));
        const uint64_t step = latticeStep(blocksX, blocksY, maxBlocks);
        // The middle block of each step x step cell of blocks
        const uint64_t offset = step / 2;

        auto accumulators = accumulatorsOf(full, nodata);
        std::vector<double> samples;
        std::vector<uint8_t> mask;
        uint64_t read = 0;
        for (uint64_t cy = 0; cy * step < blocksY; cy++)
        {
            const uint64_t by = std::min(cy * step + offset, blocksY - 1);
            for (uint64_t cx = 0; cx * step < blocksX; cx++)
            {
                cancel::throwIfCanceled();
                const uint64_t bx = std::min(cx * step + offset, blocksX - 1);
                const int x = static_cast<int>(bx) * blockWidth;
                const int y = static_cast<int>(by) * blockHeight;
                const int w = std::min(blockWidth, full.width - x);
                const int h = std::min(blockHeight, full.height - y);
                accumulateWindow(full, accumulators, x, y, w, h, samples, mask);
                read++;
            }
        }
        LOGD << "Sampled " << read << " of " << blocksX * blocksY << " blocks (" << blockWidth << "x" << blockHeight
             << ") for the statistics of the stretch";
        return finishAll(accumulators, pixelCount, true);
    }

    BandStatistics RasterStatsProvider::get(const std::string &path, GDALDatasetH ds, int band,
                                            const std::optional<double> &nodata, bool persist)
    {
        const auto entry = entryFor(path);
        std::lock_guard<std::mutex> lock(entry->mutex);

        const std::string key = std::to_string(band) + "|" + nodataKey(nodata);
        const auto cached = entry->bands.find(key);
        if (cached != entry->bands.end())
            return cached->second;

        const GDALRasterBandH hBand = GDALGetRasterBand(ds, band);
        if (hBand == nullptr)
            throw GDALException("Cannot read band " + std::to_string(band) + " of " + path);

        const std::string file = statsFileOf(path, ds);
        BandStatistics s;
        Source source;
        if (readStatsSidecar(file, band, s, nodata))
        {
            source = Source::Sidecar;
        }
        else
        {
            s = computeExactStatistics(hBand, nodata);
            source = Source::Computed;
            LOGD << "Computed band " << band << " statistics of " << path << " (" << s.min << ", "
                 << s.max << ")";
            if (persist || raster::isLibddbArtefact(io::fromUtf8(file)))
                writeStatsSidecar(file, band, s, nodata);
        }

        entry->bands[key] = s;

        std::lock_guard<std::mutex> countersLock(mutex_);
        switch (source)
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

    std::vector<std::optional<BandStatistics>> RasterStatsProvider::getAll(const std::string &path, GDALDatasetH ds,
                                                                          const std::vector<int> &bands,
                                                                          const std::optional<double> &nodata,
                                                                          bool persist,
                                                                          const std::function<void(double)> &progress)
    {
        const auto entry = entryFor(path);
        std::lock_guard<std::mutex> lock(entry->mutex);

        const std::string file = statsFileOf(path, ds);
        const auto keyOf = [&nodata](int band) { return std::to_string(band) + "|" + nodataKey(nodata); };

        std::vector<std::optional<BandStatistics>> result(bands.size());
        std::vector<size_t> missingAt; // the positions in `bands` of the bands that are not known
        std::vector<int> missing;
        uint64_t fromSidecar = 0;
        for (size_t i = 0; i < bands.size(); i++)
        {
            const int b = bands[i];
            if (GDALGetRasterBand(ds, b) == nullptr)
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
            const BandSet set = bandSetOf(ds, missing);
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

    std::vector<BandStatistics> RasterStatsProvider::getStretchStatistics(const std::string &path, GDALDatasetH ds,
                                                                          const std::vector<int> &bands,
                                                                          const std::optional<double> &nodata,
                                                                          uint64_t maxPixels)
    {
        const auto entry = entryFor(path);
        std::lock_guard<std::mutex> lock(entry->mutex);

        const std::string file = statsFileOf(path, ds);
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
            if (GDALGetRasterBand(ds, b) == nullptr)
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
                stats = sampleBandStatistics(ds, missing, nodata, maxPixels);
            }
            catch (const GDALException &)
            {
                cancel::throwIfCanceled();
                // No pixel of the sample has a value (a sparse raster): once more with a larger budget,
                // still bounded (the exact statistics when the band fits in it)
                LOGD << "No pixel of the sample of the bands of " << path << " has a value: a larger sample";
                const uint64_t larger = maxPixels > std::numeric_limits<uint64_t>::max() / SparseSampleFactor
                                            ? std::numeric_limits<uint64_t>::max()
                                            : maxPixels * SparseSampleFactor;
                stats = sampleBandStatistics(ds, missing, nodata, larger);
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
