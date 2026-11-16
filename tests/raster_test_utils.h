/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_TEST_UTILS_H
#define RASTER_TEST_UTILS_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "fs.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "raster_region.h"

namespace testutils
{
    /// Whether this build of GDAL makes the nearest half of a double. It does with the compiler's _Float16
    /// (GCC, Clang); built without it (MSVC) it rounds only the halves GDALCopyWords converts eight at a
    /// time and truncates the others, so a sample of a Float16 raster depends on where it falls in a buffer
    inline bool gdalRoundsHalves()
    {
        double v = -9999.0;
        uint16_t h = 0;
        GDALCopyWords(&v, GDT_Float64, 0, &h, GDT_Float16, 0, 1);
        return h == 0xF0E2;  // -10000, the nearest half; -9992 is the truncated one
    }

    /// The samples of `type` that differ between `a` and `b` (`bytes` long each); `first` gets the index of the
    /// first. ddb-tilers makes the nearest half of every sample: against a GDAL that does not
    /// (gdalRoundsHalves), two halves one step apart are the same sample
    inline size_t samplesThatDiffer(GDALDataType type, const uint8_t *a, const uint8_t *b, size_t bytes,
                                    size_t *first = nullptr)
    {
        const size_t size = static_cast<size_t>(GDALGetDataTypeSizeBytes(type));
        const bool halfStep = type == GDT_Float16 && !gdalRoundsHalves();
        size_t differ = 0;
        for (size_t i = 0; i + size <= bytes; i += size)
        {
            if (std::memcmp(a + i, b + i, size) == 0)
                continue;
            if (halfStep)
            {
                uint16_t ha = 0, hb = 0;
                std::memcpy(&ha, a + i, 2);
                std::memcpy(&hb, b + i, 2);
                if (((ha ^ hb) & 0x8000u) == 0 && (ha > hb ? ha - hb : hb - ha) == 1)
                    continue;
            }
            if (differ++ == 0 && first != nullptr)
                *first = i / size;
        }
        return differ;
    }

    /// Opens a raster with GDAL to look at what a test wrote or what libddb wrote (any driver, read-only):
    /// the tests look at files, libddb's host reads its rasters with ddb-tilers
    inline GDALDatasetUniquePtr openRaster(const fs::path &path)
    {
        return ddb::adoptDataset(GDALOpenEx(path.string().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    }

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
        bool alphaBand = false;  // the last band is alpha (255, or alphaValue)
        std::function<double(int x, int y)> alphaValue = [](int, int) { return 255.0; };
        std::vector<std::string> bandNames; // GDAL descriptions of the first bands, as ODM and the sensors write them
        std::function<double(int band, int x, int y)> value = [](int, int, int) { return 0.0; };
    };

    /// Writes a GeoTIFF with spec's georeference and pixel values (bands 1-based)
    fs::path createGeoRaster(const fs::path &path, const GeoRasterSpec &spec);

    /// Writes a lossless JPEG 2000 with spec's pixel values, and its georeference in GeoJP2 and
    /// GMLJP2 boxes unless georeferenced is false
    fs::path createGeoJp2(const fs::path &path, const GeoRasterSpec &spec, bool georeferenced = true);

    /// The region of a raster the tests render: bbox is [minX, minY, maxX, maxY] in srs
    inline ddb::RegionRequest regionRequest(const std::string &path, const double bbox[4], const std::string &srs,
                                            int width, int height, const std::string &format)
    {
        ddb::RegionRequest request;
        request.inputPath = path;
        std::copy(bbox, bbox + 4, request.bbox.begin());
        request.bboxSrs = srs;
        request.width = width;
        request.height = height;
        request.format = format;
        return request;
    }

    /// An encoded image (PNG, JPEG, WebP) decoded with GDAL
    struct DecodedImage
    {
        int width = 0;
        int height = 0;
        std::vector<std::vector<uint8_t>> bands;
        std::vector<GDALColorInterp> colorInterp;

        bool hasAlpha() const { return !colorInterp.empty() && colorInterp.back() == GCI_AlphaBand; }
        /// The alpha of every pixel: the alpha band, or 255 for an image without one (an encoder
        /// leaves out an alpha that is 255 everywhere)
        const std::vector<uint8_t> &alpha() const
        {
            if (hasAlpha())
                return bands.back();
            opaque_.assign(static_cast<size_t>(width) * height, 255);
            return opaque_;
        }

    private:
        mutable std::vector<uint8_t> opaque_;
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

    /// The GDAL debug messages (CPL_DEBUG) of any thread while alive, those of the drivers and of the
    /// warper: what GDAL did, such as the datasets it created ("GDALDriver::Create(MEM,,64,64,3,Float32,...)")
    class GdalDebugLog
    {
        CPLErrorHandler previous_;
        std::string previousDebug_;

    public:
        GdalDebugLog();
        ~GdalDebugLog();
        /// The messages that contain text
        std::vector<std::string> containing(const std::string &text) const;
    };

    /// Copies a buffer allocated by GDAL (VSIMalloc) and frees it
    std::vector<uint8_t> takeBuffer(uint8_t *buffer, int size);
}

#endif // RASTER_TEST_UTILS_H
