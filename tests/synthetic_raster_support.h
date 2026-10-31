/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SYNTHETIC_RASTER_SUPPORT_H
#define SYNTHETIC_RASTER_SUPPORT_H

// The synthetic rasters of the golden tests: a five-band multispectral raster (R, G, B, RedEdge,
// NIR) in Web Mercator with nodata patches, and small GeoTIFFs of any band layout.

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "synthetic_dem_support.h"
#include "gdal_inc.h"
#include "raster_test_utils.h"

namespace golden
{
    // Five bands of reflectance (R, G, B, RedEdge, NIR) of a 128 x 128 raster in Web
    // Mercator. Only exact IEEE operations, so every platform writes the same floats.
    inline double reflectance(int band, int x, int y)
    {
        const int cell = (x * (2 + band) + y * (11 - band) + 17 * band) % 97;
        return 0.05 + (cell / 97.0) * 0.7;
    }

    inline constexpr double kOriginX = 1113000.0;
    inline constexpr double kOriginY = 5621000.0;
    inline constexpr double kPixel = 4.0;
    inline constexpr int kSize = 128;

    // nodata in band 3 over (20..39, 20..39) and in band 5 over (80..99, 60..79)
    inline double reflectanceWithNodata(int band, int x, int y, double patch = kNodata)
    {
        if (band == 3 && x >= 20 && x < 40 && y >= 20 && y < 40)
            return patch;
        if (band == 5 && x >= 80 && x < 100 && y >= 60 && y < 80)
            return patch;
        return reflectance(band, x, y);
    }

    inline void setNodata(const fs::path &path, double nodata)
    {
        std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(path.string().c_str(), GA_Update)));
        ASSERT_NE(ds, nullptr);
        for (int b = 1; b <= ds->GetRasterCount(); b++)
            ds->GetRasterBand(b)->SetNoDataValue(nodata);
    }

    // The five-band raster; `declared` is its nodata value and `patch` the value of its nodata
    // pixels (they differ for a nodata value that a float cannot hold, and for NaN)
    inline fs::path makeMultispectral(const fs::path &path, double declared = kNodata, double patch = kNodata,
                                      GDALDataType type = GDT_Float32)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = kSize;
        spec.bands = 5;
        spec.type = type;
        spec.epsg = 3857;
        spec.originX = kOriginX;
        spec.originY = kOriginY;
        spec.pixelSize = kPixel;
        spec.value = [patch](int band, int x, int y) { return reflectanceWithNodata(band, x, y, patch); };
        testutils::createGeoRaster(path, spec);
        setNodata(path, declared);
        return path;
    }

    // A GeoTIFF of size x size pixels of 1 m in UTM 32N whose band b holds value(b, x, y)
    // (scaled to 0..255 for Byte), with the nodata value of every band
    inline fs::path writeRaster(const fs::path &path, int bands, GDALDataType type, bool alpha,
                                std::optional<double> nodata, int size,
                                const std::function<double(int band, int x, int y)> &value)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = size;
        spec.bands = bands;
        spec.type = type;
        spec.alphaBand = alpha;
        spec.epsg = 32632;
        spec.originX = 500000.0;
        spec.originY = 4500000.0;
        spec.pixelSize = 1.0;
        const double scale = type == GDT_Byte ? 255.0 : 1.0;
        spec.value = [scale, value](int band, int x, int y) {
            return std::floor(value(band, x, y) * scale * 100.0) / 100.0;
        };
        testutils::createGeoRaster(path, spec);
        if (nodata)
            setNodata(path, *nodata);
        if (bands == 4 && !alpha)
        {
            // RGB + NIR: band 4 is not an alpha channel
            std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(path.string().c_str(), GA_Update)));
            for (int b = 1; b <= 4; b++)
                ds->GetRasterBand(b)->SetColorInterpretation(GCI_GrayIndex);
        }
        return path;
    }

    // Bands of diagonal ramps: their values repeat along lines, so they are no content to align
    inline fs::path makeRaster(const fs::path &path, int bands, GDALDataType type, bool alpha, std::optional<double> nodata,
                        int size = 96)
    {
        return writeRaster(path, bands, type, alpha, nodata, size, [](int band, int x, int y) {
            return ((x * (3 + band) + y * (7 - band) + 13 * band) % 251) / 250.0;
        });
    }

    // Broadband noise in [0, 1) of pixel (x, y), from integer operations only (every platform
    // writes the same values)
    inline double pixelNoise(int x, int y)
    {
        uint32_t h = static_cast<uint32_t>(y) * 73856093u ^ static_cast<uint32_t>(x) * 19349663u;
        h ^= h >> 13;
        h *= 0x85ebca6bu;
        h ^= h >> 16;
        return static_cast<double>(h & 0xFFFFFFu) / static_cast<double>(0x1000000u);
    }

    // Bands of noise: a raster that alignment can find a shift of (one of makeRaster is the same
    // after many shifts)
    inline fs::path makeTexturedRaster(const fs::path &path, int bands, GDALDataType type,
                                       std::optional<double> nodata, int size = 96)
    {
        return writeRaster(path, bands, type, false, nodata, size,
                           [](int band, int x, int y) { return pixelNoise(x + 7 * band, y); });
    }
}

#endif // SYNTHETIC_RASTER_SUPPORT_H
