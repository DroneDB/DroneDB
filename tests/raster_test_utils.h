/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_TEST_UTILS_H
#define RASTER_TEST_UTILS_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "fs.h"
#include "gdal_inc.h"

namespace testutils
{
    struct GeoRasterSpec
    {
        int width = 512;
        int height = 512;
        int bands = 3;
        GDALDataType type = GDT_Byte;
        int epsg = 3857;
        double originX = 1000.0; // upper-left corner
        double originY = 1000.0;
        double pixelSize = 1.0;
        bool alphaBand = false;  // the last band is alpha (255)
        std::function<double(int band, int x, int y)> value = [](int, int, int) { return 0.0; };
    };

    /// Writes a GeoTIFF with spec's georeference and pixel values (bands 1-based)
    fs::path createGeoRaster(const fs::path &path, const GeoRasterSpec &spec);

    /// An encoded image (PNG, JPEG, WebP) decoded with GDAL
    struct DecodedImage
    {
        int width = 0;
        int height = 0;
        std::vector<std::vector<uint8_t>> bands;
        std::vector<GDALColorInterp> colorInterp;

        bool hasAlpha() const { return !colorInterp.empty() && colorInterp.back() == GCI_AlphaBand; }
        const std::vector<uint8_t> &alpha() const { return bands.back(); }
    };

    DecodedImage decodeImage(const uint8_t *data, size_t size);
    DecodedImage decodeImage(const std::vector<uint8_t> &data);
    DecodedImage decodeImageFile(const fs::path &path);

    /// Counts the files GDAL opens as datasets (GDALOpen/GDALOpenEx successes, not
    /// /vsimem/ files) while alive, on any thread, through GDAL debug messages
    class GdalOpenCounter
    {
        CPLErrorHandler previous_;
        std::string previousDebug_;

    public:
        GdalOpenCounter();
        ~GdalOpenCounter();
        int count() const;
    };

    /// Copies a buffer allocated by GDAL (VSIMalloc) and frees it
    std::vector<uint8_t> takeBuffer(uint8_t *buffer, int size);
}

#endif // RASTER_TEST_UTILS_H
