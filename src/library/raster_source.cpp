/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_source.h"

#include <algorithm>

#include "cancellation.h"
#include "exceptions.h"
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
            bool isAlpha(int band) const override
            {
                return GDALGetRasterColorInterpretation(bandOf(band)) == GCI_AlphaBand;
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
            bool isAlpha(int band) const override { return bandOf(band).colorInterp == "Alpha"; }
            std::optional<double> scale(int band) const override { return bandOf(band).scale; }
            std::optional<double> offset(int band) const override { return bandOf(band).offset; }
            std::string unit(int band) const override { return bandOf(band).unit; }
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
