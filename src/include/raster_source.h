/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_SOURCE_H
#define RASTER_SOURCE_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gdal_inc.h"
#include "raster_utils.h"

namespace ddb
{
    namespace tilers
    {
        class Raster;
    }

    /// The bands of a raster that libddb reads its values from (statistics, formulas, band values): a dataset GDAL
    /// opened (the raster helper, which reads the files of users; the renders and exports until they read through
    /// ddb-tilers) or a raster ddb-tilers reads (the host: the COG of a build, a normalized copy). Both report and
    /// read the same, so what is computed from them is the same to the bit.
    class RasterSource
    {
    public:
        virtual ~RasterSource() = default;
        /// The file the statistics are of (their sidecar goes next to it); empty when it is not a file
        virtual std::string file() const = 0;
        virtual int width() const = 0;
        virtual int height() const = 0;
        virtual int bandCount() const = 0;
        /// The NoData value of a band (1-based)
        virtual BandNodata nodata(int band) const = 0;
        /// The data type of a band
        virtual GDALDataType dataType(int band) const = 0;
        /// Whether a band holds Float32 samples (its NoData value is compared rounded to float)
        bool isFloat32(int band) const { return dataType(band) == GDT_Float32; }
        /// GDAL's name of the colour interpretation of a band ("Red", "Gray", "Alpha", "Undefined"...)
        virtual std::string colorInterp(int band) const = 0;
        /// Whether a band is an alpha band (its colour interpretation)
        bool isAlpha(int band) const { return colorInterp(band) == "Alpha"; }
        /// The scale and offset of a band, each when it has one
        virtual std::optional<double> scale(int band) const = 0;
        virtual std::optional<double> offset(int band) const = 0;
        /// The unit type of a band, "" without one
        virtual std::string unit(int band) const = 0;
        /// The description of a band, "" without one
        virtual std::string description(int band) const = 0;
        /// An item of the default metadata domain of a band (GDALGetMetadataItem: the name in any case)
        virtual std::optional<std::string> metadataItem(int band, const std::string &name) const = 0;
        /// Whether a band has an alpha band or a mask of the dataset (not a NoData mask): the bands that have one
        /// share it (GMF_PER_DATASET)
        virtual bool hasMask(int band) const = 0;
        /// The block size of the first band
        virtual void blockSize(int &width, int &height) const = 0;
        /// The overviews of the first band (GDALGetOverviewCount)
        virtual int overviewCount() const = 0;
        /// The size of overview `index` (GDALGetOverview) of every band of `bands`; none when a band lacks it or
        /// has it in another size
        virtual std::optional<std::pair<int, int>> overviewSize(const std::vector<int> &bands, int index) const = 0;
        /// Reads `window` of `level` (0: the raster, read at outWidth x outHeight; i: overview i - 1, read pixel
        /// for pixel) of `bands` into planes of Float64 samples, one band after the other, and in `mask` (outPixels
        /// bytes, when one of the bands hasMask, else left untouched) their mask: 0 where a pixel is transparent.
        /// @throws GDALException when it cannot be read
        virtual void read(int level, const RasterWindow &window, const std::vector<int> &bands, double *samples,
                          uint8_t *mask) const = 0;
        /// The same, at full resolution, into Float32 samples
        virtual void read(const RasterWindow &window, const std::vector<int> &bands, float *samples,
                          uint8_t *mask) const = 0;
    };

    /// The source seen with the NoData value `nodata` instead of the one of every band (FR-RAS-31), as the VRT of
    /// withNodataOverride (raster_utils.h) shows a dataset: the masks follow the new value (no band keeps an alpha or
    /// a dataset mask: the NoData value decides), the samples are those of the source. It names no file, so its
    /// statistics are kept in memory only. The source must outlive it.
    std::unique_ptr<RasterSource> withNodataOverride(const RasterSource &source, double nodata);

    /// The alpha band, the dataset mask and the NoData value of each of the first `bandCount` bands of the source
    /// (detectBandNodata of a GDAL dataset)
    RasterBandNodata detectBandNodata(const RasterSource &source, int bandCount);

    /// The source of a dataset GDAL opened, which must outlive it
    std::unique_ptr<RasterSource> rasterSourceOf(GDALDatasetH ds);
    /// The source of a raster ddb-tilers reads, which must outlive it
    std::unique_ptr<RasterSource> rasterSourceOf(const tilers::Raster &raster);

}

#endif // RASTER_SOURCE_H
