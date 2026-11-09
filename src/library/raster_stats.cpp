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

    BandStatistics computeExactStatistics(GDALRasterBandH hBand, const std::optional<double> &nodata)
    {
        const int width = GDALGetRasterBandXSize(hBand);
        const int height = GDALGetRasterBandYSize(hBand);

        BandNodata nd = readBandNodata(hBand);
        if (nodata)
            nd = BandNodata{true, *nodata};

        // The transparency that is not the NoData value: an alpha band or a mask of the dataset
        const int maskFlags = GDALGetMaskFlags(hBand);
        GDALRasterBandH maskBand = nullptr;
        if ((maskFlags & (GMF_ALPHA | GMF_PER_DATASET)) != 0 && (maskFlags & (GMF_NODATA | GMF_ALL_VALID)) == 0)
            maskBand = GDALGetMaskBand(hBand);

        const bool floatSamples = GDALGetRasterDataType(hBand) == GDT_Float32;

        BandStatistics s;
        s.pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        double mean = 0.0, m2 = 0.0;
        double lo = std::numeric_limits<double>::infinity();
        double hi = -std::numeric_limits<double>::infinity();
        uint64_t n = 0;

        // A few million pixels at a time: the pass stops when the call is canceled
        const int rowsPerChunk = std::max(1, static_cast<int>((1 << 22) / std::max(1, width)));
        std::vector<double> samples;
        std::vector<uint8_t> mask;
        for (int y = 0; y < height; y += rowsPerChunk)
        {
            cancel::throwIfCanceled();
            const int rows = std::min(rowsPerChunk, height - y);
            const size_t count = static_cast<size_t>(rows) * width;
            samples.resize(count);
            cancel::GdalProgress progress;
            GDALRasterIOExtraArg extra = progress.rasterIOExtraArg();
            if (GDALRasterIOEx(hBand, GF_Read, 0, y, width, rows, samples.data(), width, rows, GDT_Float64, 0, 0,
                               &extra) != CE_None)
            {
                cancel::throwIfCanceled();
                throw GDALException("Cannot read the band to compute its statistics");
            }
            if (maskBand != nullptr)
            {
                mask.resize(count);
                GDALRasterIOExtraArg maskExtra = progress.rasterIOExtraArg();
                if (GDALRasterIOEx(maskBand, GF_Read, 0, y, width, rows, mask.data(), width, rows, GDT_Byte, 0, 0,
                                   &maskExtra) != CE_None)
                {
                    cancel::throwIfCanceled();
                    throw GDALException("Cannot read the mask of the band to compute its statistics");
                }
            }

            for (size_t i = 0; i < count; i++)
            {
                const double v = samples[i];
                // A Float32 band holds its NoData value rounded to float (GDAL's rule, and DemContext::readRaw's):
                // 1e-7 matches the float 1e-7 of the band
                const bool isNd = floatSamples ? isNodata(static_cast<float>(v), nd) : isNodata(v, nd);
                if (!std::isfinite(v) || isNd || (maskBand != nullptr && mask[i] == 0))
                    continue;
                n++;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                const double delta = v - mean;
                mean += delta / static_cast<double>(n);
                m2 += delta * (v - mean);
            }
        }

        if (n == 0)
            throw GDALException("Cannot compute statistics: no pixel of the band has a value");
        s.min = lo;
        s.max = hi;
        s.mean = mean;
        s.stdDev = std::sqrt(m2 / static_cast<double>(n));
        s.validCount = n;
        return s;
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
