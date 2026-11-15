/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_source.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "cancellation.h"
#include "exceptions.h"
#include "fileversion.h"
#include "mio.h"
#include "tilers_raster.h"

namespace ddb
{
    namespace
    {
        // A dataset GDAL opened
        class GdalRasterSource final : public RasterSource
        {
        public:
            explicit GdalRasterSource(GDALDatasetH ds) : ds_(ds) {}

            std::string file() const override
            {
                const char *description = GDALGetDescription(ds_);
                return description != nullptr ? description : "";
            }
            int width() const override { return GDALGetRasterXSize(ds_); }
            int height() const override { return GDALGetRasterYSize(ds_); }
            int bandCount() const override { return GDALGetRasterCount(ds_); }
            BandNodata nodata(int band) const override { return readBandNodata(bandOf(band)); }
            GDALDataType dataType(int band) const override { return GDALGetRasterDataType(bandOf(band)); }
            std::string colorInterp(int band) const override
            {
                return GDALGetColorInterpretationName(GDALGetRasterColorInterpretation(bandOf(band)));
            }
            std::optional<double> scale(int band) const override
            {
                int has = FALSE;
                const double v = GDALGetRasterScale(bandOf(band), &has);
                return has ? std::optional<double>(v) : std::nullopt;
            }
            std::optional<double> offset(int band) const override
            {
                int has = FALSE;
                const double v = GDALGetRasterOffset(bandOf(band), &has);
                return has ? std::optional<double>(v) : std::nullopt;
            }
            std::string unit(int band) const override
            {
                const char *u = GDALGetRasterUnitType(bandOf(band));
                return u != nullptr ? u : "";
            }
            std::string description(int band) const override { return GDALGetDescription(bandOf(band)); }
            std::optional<std::string> metadataItem(int band, const std::string &name) const override
            {
                const char *v = GDALGetMetadataItem(bandOf(band), name.c_str(), nullptr);
                return v != nullptr ? std::optional<std::string>(v) : std::nullopt;
            }
            bool hasMask(int band) const override { return maskBandOf(bandOf(band)) != nullptr; }
            void blockSize(int &width, int &height) const override { GDALGetBlockSize(bandOf(1), &width, &height); }
            int overviewCount() const override { return GDALGetOverviewCount(bandOf(1)); }

            std::optional<std::pair<int, int>> overviewSize(const std::vector<int> &bands, int index) const override
            {
                std::optional<std::pair<int, int>> size;
                for (const int b : bands)
                {
                    const GDALRasterBandH ov = GDALGetOverview(bandOf(b), index);
                    if (ov == nullptr)
                        return std::nullopt;
                    const std::pair<int, int> s{GDALGetRasterBandXSize(ov), GDALGetRasterBandYSize(ov)};
                    if (size && *size != s)
                        return std::nullopt;
                    size = s;
                }
                return size;
            }

            void read(int level, const RasterWindow &w, const std::vector<int> &bands, double *samples,
                      uint8_t *mask) const override
            {
                readAs(level, w, bands, samples, GDT_Float64, mask);
            }

            void read(const RasterWindow &w, const std::vector<int> &bands, float *samples,
                      uint8_t *mask) const override
            {
                readAs(0, w, bands, samples, GDT_Float32, mask);
            }

        private:
            template <typename T>
            void readAs(int level, const RasterWindow &w, const std::vector<int> &bands, T *samples,
                        GDALDataType type, uint8_t *mask) const
            {
                std::vector<GDALRasterBandH> levelBands;
                for (const int b : bands)
                    levelBands.push_back(level == 0 ? bandOf(b) : GDALGetOverview(bandOf(b), level - 1));
                const size_t count = w.outPixels();
                cancel::GdalProgress progress;
                // One read when the bands are the bands of one dataset (the raster, or the overview level of a
                // GeoTIFF): a file whose blocks hold every band is decoded once
                GDALDatasetH levelDs = level == 0 ? ds_ : GDALGetBandDataset(levelBands.front());
                std::vector<int> numbers;
                for (size_t i = 0; levelDs != nullptr && i < levelBands.size(); i++)
                {
                    const int number = GDALGetBandNumber(levelBands[i]);
                    if (GDALGetBandDataset(levelBands[i]) != levelDs || number <= 0 ||
                        GDALGetRasterBand(levelDs, number) != levelBands[i])
                        levelDs = nullptr;
                    else
                        numbers.push_back(number);
                }
                if (levelDs != nullptr)
                {
                    GDALRasterIOExtraArg extra = progress.rasterIOExtraArg();
                    if (GDALDatasetRasterIOEx(levelDs, GF_Read, w.x, w.y, w.xsize, w.ysize, samples, w.outWidth,
                                              w.outHeight, type, static_cast<int>(numbers.size()),
                                              numbers.data(), 0, 0, 0, &extra) != CE_None)
                    {
                        cancel::throwIfCanceled();
                        throw GDALException("Cannot read the bands of the raster");
                    }
                }
                else
                {
                    for (size_t i = 0; i < levelBands.size(); i++)
                    {
                        GDALRasterIOExtraArg extra = progress.rasterIOExtraArg();
                        if (GDALRasterIOEx(levelBands[i], GF_Read, w.x, w.y, w.xsize, w.ysize, samples + i * count,
                                           w.outWidth, w.outHeight, type, 0, 0, &extra) != CE_None)
                        {
                            cancel::throwIfCanceled();
                            throw GDALException("Cannot read a band of the raster");
                        }
                    }
                }

                // The mask the masked bands share (GMF_PER_DATASET); all valid when a level has none
                if (mask == nullptr)
                    return;
                std::fill(mask, mask + count, uint8_t{255});
                for (const GDALRasterBandH band : levelBands)
                {
                    const GDALRasterBandH maskBand = maskBandOf(band);
                    if (maskBand == nullptr)
                        continue;
                    GDALRasterIOExtraArg extra = progress.rasterIOExtraArg();
                    if (GDALRasterIOEx(maskBand, GF_Read, w.x, w.y, w.xsize, w.ysize, mask, w.outWidth, w.outHeight,
                                       GDT_Byte, 0, 0, &extra) != CE_None)
                    {
                        cancel::throwIfCanceled();
                        throw GDALException("Cannot read the mask of the raster");
                    }
                    break;
                }
            }

            GDALRasterBandH bandOf(int band) const
            {
                const GDALRasterBandH hBand = GDALGetRasterBand(ds_, band);
                if (hBand == nullptr)
                    throw GDALException("Cannot read band " + std::to_string(band) + " of the raster");
                return hBand;
            }

            // The transparency that is not the NoData value: an alpha band or a mask of the dataset
            static GDALRasterBandH maskBandOf(GDALRasterBandH band)
            {
                const int flags = GDALGetMaskFlags(band);
                if ((flags & (GMF_ALPHA | GMF_PER_DATASET)) != 0 && (flags & (GMF_NODATA | GMF_ALL_VALID)) == 0)
                    return GDALGetMaskBand(band);
                return nullptr;
            }

            GDALDatasetH ds_;
        };

        // A raster ddb-tilers reads
        class TilersRasterSource final : public RasterSource
        {
        public:
            explicit TilersRasterSource(const tilers::Raster &raster) : raster_(raster) {}

            std::string file() const override { return io::toUtf8(raster_.file()); }
            int width() const override { return raster_.width(); }
            int height() const override { return raster_.height(); }
            int bandCount() const override { return raster_.bandCount(); }
            BandNodata nodata(int band) const override
            {
                const auto &nd = bandOf(band).nodata;
                return nd ? BandNodata{true, *nd} : BandNodata{};
            }
            GDALDataType dataType(int band) const override
            {
                bandOf(band);
                return GDALGetDataTypeByName(raster_.dataType().c_str());
            }
            std::string colorInterp(int band) const override { return bandOf(band).colorInterp; }
            std::optional<double> scale(int band) const override { return bandOf(band).scale; }
            std::optional<double> offset(int band) const override { return bandOf(band).offset; }
            std::string unit(int band) const override { return bandOf(band).unit; }
            std::string description(int band) const override { return bandOf(band).description; }
            std::optional<std::string> metadataItem(int band, const std::string &name) const override
            {
                for (const auto &[key, value] : bandOf(band).metadata)
                    if (EQUAL(key.c_str(), name.c_str()))
                        return value;
                return std::nullopt;
            }
            bool hasMask(int band) const override
            {
                bandOf(band);
                return raster_.hasMask(band);
            }
            void blockSize(int &width, int &height) const override
            {
                width = raster_.blockWidth();
                height = raster_.blockHeight();
            }
            int overviewCount() const override { return static_cast<int>(raster_.overviews().size()); }

            std::optional<std::pair<int, int>> overviewSize(const std::vector<int> &bands, int index) const override
            {
                for (const int b : bands)
                    bandOf(b);
                // The overviews of a TIFF are levels of the whole file: every band has them
                if (index < 0 || index >= overviewCount())
                    return std::nullopt;
                const auto &size = raster_.overviews()[static_cast<size_t>(index)];
                return std::pair<int, int>{size[0], size[1]};
            }

            void read(int level, const RasterWindow &w, const std::vector<int> &bands, double *samples,
                      uint8_t *mask) const override
            {
                guarded([&] {
                    if (level == 0)
                        raster_.readMasked(w.x, w.y, w.xsize, w.ysize, w.outWidth, w.outHeight, bands, samples, mask);
                    else
                        raster_.readLevel(level, w.x, w.y, w.xsize, w.ysize, bands, samples, mask);
                });
            }

            void read(const RasterWindow &w, const std::vector<int> &bands, float *samples,
                      uint8_t *mask) const override
            {
                guarded([&] {
                    raster_.readMasked(w.x, w.y, w.xsize, w.ysize, w.outWidth, w.outHeight, bands, samples, mask);
                });
            }

        private:
            // A read that fails is a GDALException, as GDAL's
            template <typename Call> static void guarded(const Call &call)
            {
                try
                {
                    call();
                }
                catch (const GDALException &)
                {
                    throw;
                }
                catch (const std::exception &e)
                {
                    throw GDALException(std::string("Cannot read the bands of the raster: ") + e.what());
                }
            }

            const tilers::Raster::Band &bandOf(int band) const
            {
                if (band < 1 || band > raster_.bandCount())
                    throw GDALException("Cannot read band " + std::to_string(band) + " of the raster");
                return raster_.band(band);
            }

            const tilers::Raster &raster_;
        };
    }

    std::unique_ptr<RasterSource> rasterSourceOf(GDALDatasetH ds) { return std::make_unique<GdalRasterSource>(ds); }

    std::unique_ptr<RasterSource> rasterSourceOf(const tilers::Raster &raster)
    {
        return std::make_unique<TilersRasterSource>(raster);
    }


    namespace
    {
        class NodataOverrideSource final : public RasterSource
        {
        public:
            NodataOverrideSource(const RasterSource &source, double nodata) : source_(source), nodata_(nodata) {}

            std::string file() const override { return std::string(); }
            int width() const override { return source_.width(); }
            int height() const override { return source_.height(); }
            int bandCount() const override { return source_.bandCount(); }
            BandNodata nodata(int band) const override
            {
                source_.dataType(band);  // checks the band
                return BandNodata{true, nodata_};
            }
            GDALDataType dataType(int band) const override { return source_.dataType(band); }
            std::string colorInterp(int band) const override { return source_.colorInterp(band); }
            std::optional<double> scale(int band) const override { return source_.scale(band); }
            std::optional<double> offset(int band) const override { return source_.offset(band); }
            std::string unit(int band) const override { return source_.unit(band); }
            std::string description(int band) const override { return source_.description(band); }
            std::optional<std::string> metadataItem(int band, const std::string &name) const override
            {
                return source_.metadataItem(band, name);
            }
            bool hasMask(int band) const override
            {
                source_.dataType(band);
                return false;
            }
            void blockSize(int &width, int &height) const override { source_.blockSize(width, height); }
            int overviewCount() const override { return source_.overviewCount(); }
            std::optional<std::pair<int, int>> overviewSize(const std::vector<int> &bands, int index) const override
            {
                return source_.overviewSize(bands, index);
            }
            // No band has a mask: none is asked for
            void read(int level, const RasterWindow &window, const std::vector<int> &bands, double *samples,
                      uint8_t *) const override
            {
                source_.read(level, window, bands, samples, nullptr);
            }
            void read(const RasterWindow &window, const std::vector<int> &bands, float *samples,
                      uint8_t *) const override
            {
                source_.read(window, bands, samples, nullptr);
            }

        private:
            const RasterSource &source_;
            double nodata_;
        };
    }

    namespace
    {
        // gdalwarp's MEM dataset of a warp
        class WarpedImageSource final : public RasterSource
        {
        public:
            explicit WarpedImageSource(const tilers::WarpedImage &image) : image_(image) {}

            std::string file() const override { return std::string(); }
            int width() const override { return image_.width; }
            int height() const override { return image_.height; }
            int bandCount() const override { return image_.bandCount(); }
            BandNodata nodata(int band) const override
            {
                if (band == image_.alphaBand || !image_.nodata)
                    return {};
                return {true, *image_.nodata};
            }
            GDALDataType dataType(int /*band*/) const override { return GDALGetDataTypeByName(image_.dataType.c_str()); }
            std::string colorInterp(int band) const override { return image_.colorInterp.at(static_cast<size_t>(band - 1)); }
            std::optional<double> scale(int) const override { return std::nullopt; }
            std::optional<double> offset(int) const override { return std::nullopt; }
            std::string unit(int) const override { return std::string(); }
            std::string description(int) const override { return std::string(); }
            std::optional<std::string> metadataItem(int, const std::string &) const override { return std::nullopt; }
            bool hasMask(int band) const override { return maskBand(band) > 0; }
            void blockSize(int &w, int &h) const override
            {
                w = image_.width;
                h = image_.height;
            }
            int overviewCount() const override { return 0; }
            std::optional<std::pair<int, int>> overviewSize(const std::vector<int> &, int) const override
            {
                return std::nullopt;
            }
            void read(int level, const RasterWindow &window, const std::vector<int> &bands, double *samples,
                      uint8_t *mask) const override
            {
                if (level != 0)
                    throw InvalidArgsException("A warped image has no overviews");
                readInto(window, bands, samples, mask);
            }
            void read(const RasterWindow &window, const std::vector<int> &bands, float *samples,
                      uint8_t *mask) const override
            {
                readInto(window, bands, samples, mask);
            }

        private:
            // GDALRasterBand's default mask: a band without nodata of 2 or 4 bands whose last one is alpha is
            // masked by it (GMF_ALPHA | GMF_PER_DATASET); 0 when it has none
            int maskBand(int band) const
            {
                const int n = image_.bandCount();
                if (nodata(band).has || band == n || (n != 2 && n != 4))
                    return 0;
                return image_.colorInterp.back() == "Alpha" ? n : 0;
            }

            template <typename T>
            void readInto(const RasterWindow &window, const std::vector<int> &bands, T *samples, uint8_t *mask) const
            {
                if (window.outWidth != window.xsize || window.outHeight != window.ysize)
                    throw InvalidArgsException("A warped image is read pixel for pixel");
                std::vector<T> plane;
                const size_t count = static_cast<size_t>(window.xsize) * window.ysize;
                const auto copy = [&](int band, const auto &put) {
                    image_.readBand(band, plane);
                    for (int y = 0; y < window.ysize; y++)
                        for (int x = 0; x < window.xsize; x++)
                            put(static_cast<size_t>(y) * window.xsize + x,
                                plane[static_cast<size_t>(window.y + y) * image_.width + (window.x + x)]);
                };
                for (size_t i = 0; i < bands.size(); i++)
                    copy(bands[i], [&](size_t at, T v) { samples[i * count + at] = v; });
                const int masked = bands.empty() ? 0 : maskBand(bands.front());
                if (mask != nullptr && masked > 0)
                    // The alpha band read as Byte (GDALCopyWords: clamped, rounded)
                    copy(masked, [&](size_t at, T v) {
                        mask[at] = !(v > 0) ? 0 : v >= 255 ? 255 : static_cast<uint8_t>(v + T(0.5));
                    });
            }

            const tilers::WarpedImage &image_;
        };
    }

    std::unique_ptr<RasterSource> rasterSourceOf(const tilers::WarpedImage &image)
    {
        return std::make_unique<WarpedImageSource>(image);
    }

    namespace
    {
        // The largest finite value of band `band` in a grid of `grid` x `grid` windows, one in the block under
        // each grid point (at most kAlphaSampleWindow pixels a side, centred on the point and kept in that block,
        // so a window decodes one block: one row of a strip-organised raster), read at full resolution. 0 when
        // none of them holds a value above 0
        constexpr int kAlphaSampleWindow = 256;

        double sampledFullResolutionMaximum(const RasterSource &source, int band, int grid)
        {
            const int w = source.width(), h = source.height();
            int bw = 0, bh = 0;
            source.blockSize(bw, bh);
            if (bw <= 0 || bh <= 0)
                bw = bh = kAlphaSampleWindow;
            std::vector<double> buffer;
            double maximum = 0.0;
            for (int gy = 0; gy < grid; gy++)
                for (int gx = 0; gx < grid; gx++)
                {
                    const int cx = static_cast<int>((gx + 0.5) * w / grid);
                    const int cy = static_cast<int>((gy + 0.5) * h / grid);
                    // The block under the grid point, cut at the raster's edge
                    const int bx0 = cx / bw * bw, by0 = cy / bh * bh;
                    const int bx1 = std::min(bx0 + bw, w), by1 = std::min(by0 + bh, h);
                    const int ww = std::min(bx1 - bx0, kAlphaSampleWindow), wh = std::min(by1 - by0, kAlphaSampleWindow);
                    const int x0 = std::clamp(cx - ww / 2, bx0, bx1 - ww), y0 = std::clamp(cy - wh / 2, by0, by1 - wh);
                    buffer.resize(static_cast<size_t>(ww) * wh);
                    source.read(0, RasterWindow{x0, y0, ww, wh, ww, wh}, {band}, buffer.data(), nullptr);
                    for (const double v : buffer)
                        if (std::isfinite(v) && v > maximum)
                            maximum = v;
                }
            return maximum;
        }

        // The maximum of every pixel of the band but its nodata value (GDALComputeRasterMinMax, exact), by rows
        double exactMaximum(const RasterSource &source, int band)
        {
            const BandNodata nodata = source.nodata(band);
            const int w = source.width(), h = source.height();
            constexpr int kRows = 256;
            std::vector<double> buffer;
            double maximum = -std::numeric_limits<double>::infinity();
            for (int y = 0; y < h; y += kRows)
            {
                const int rows = std::min(kRows, h - y);
                buffer.resize(static_cast<size_t>(w) * rows);
                source.read(0, RasterWindow{0, y, w, rows, w, rows}, {band}, buffer.data(), nullptr);
                for (const double v : buffer)
                    if (std::isfinite(v) && !isNodata(v, nodata) && v > maximum)
                        maximum = v;
            }
            return maximum;
        }
    }

    namespace
    {
        // The opaque values derived for the alpha bands of files, by version of the file: the index renders of a
        // map ask for the one of their raster on every tile (a sample of 16 windows)
        class AlphaOpaqueValues
        {
        public:
            std::optional<double> find(const std::string &key)
            {
                std::lock_guard lock(mutex_);
                const auto it = values_.find(key);
                return it == values_.end() ? std::nullopt : std::optional<double>(it->second);
            }
            void put(const std::string &key, double value)
            {
                std::lock_guard lock(mutex_);
                if (values_.size() >= kMax)
                    values_.clear();
                values_[key] = value;
            }

        private:
            static constexpr size_t kMax = 1024;
            std::mutex mutex_;
            std::unordered_map<std::string, double> values_;
        };

        AlphaOpaqueValues &opaqueValues()
        {
            static AlphaOpaqueValues values;
            return values;
        }

        double deriveAlphaOpaqueValue(const RasterSource &source, int band);
    }

    double alphaOpaqueValue(const RasterSource &source, int band)
    {
        const std::string file = source.file();
        if (file.empty())
            return deriveAlphaOpaqueValue(source, band);
        const std::string key = file + "|" + fileVersion(io::fromUtf8(file)) + "|" + std::to_string(band);
        if (const auto known = opaqueValues().find(key))
            return *known;
        const double value = deriveAlphaOpaqueValue(source, band);
        opaqueValues().put(key, value);
        return value;
    }

    namespace
    {
        double deriveAlphaOpaqueValue(const RasterSource &source, int band)
        {
            // Not GDAL's approximate maximum: it reads the overviews, and the cubic overviews of a COG ring above the
            // opaque value at the edge of the data (266 for 255, which made 511 opaque)
            double maximum = 0.0;
            bool sampled = false;
            try
            {
                for (const int grid : {4, 16})
                {
                    maximum = sampledFullResolutionMaximum(source, band, grid);
                    sampled = true;
                    if (maximum > 0.0)
                        break;
                }
                if (!(maximum > 0.0))
                    maximum = exactMaximum(source, band);
            }
            catch (const AppException &)
            {
                // A band with nothing to read is not an error here
                sampled = false;
            }
            if (sampled && maximum > 0.0)
            {
                // The all-ones value that holds the maximum: 2^bits - 1
                const double bits = std::ceil(std::log2(std::ceil(maximum) + 1.0));
                return std::ldexp(1.0, static_cast<int>(bits)) - 1.0;
            }
            const GDALDataType type = source.dataType(band);
            if (GDALDataTypeIsFloating(type))
                return 1.0;
            const int bits = GDALGetDataTypeSizeBits(type) - (GDALDataTypeIsSigned(type) ? 1 : 0);
            return std::ldexp(1.0, bits) - 1.0;
        }

    }

    std::unique_ptr<RasterSource> withNodataOverride(const RasterSource &source, double nodata)
    {
        return std::make_unique<NodataOverrideSource>(source, nodata);
    }

    RasterBandNodata detectBandNodata(const RasterSource &source, int bandCount)
    {
        RasterBandNodata info;
        info.alphaBandIdx = -1;
        info.bands.resize(bandCount);
        for (int b = 0; b < bandCount; b++)
        {
            if (source.isAlpha(b + 1))
                info.alphaBandIdx = b;
            info.bands[b] = source.nodata(b + 1);
        }
        // A mask kept for the whole dataset (without an alpha band, the mask of the first band is one when it is
        // neither all valid nor its NoData value); a nodata mask is the nodata compare already
        if (info.alphaBandIdx < 0 && bandCount > 0)
            info.datasetMask = source.hasMask(1);
        return info;
    }
}
