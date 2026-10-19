/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_test_utils.h"

#include <cstring>
#include <mutex>
#include <stdexcept>

#include "utils.h"

namespace testutils
{
    namespace
    {
        std::atomic<int> openCount{0};

        void CPL_STDCALL countOpens(CPLErr level, CPLErrorNum, const char *msg)
        {
            // In-memory files (encoded tiles that drivers reopen) are not raster opens
            if (level == CE_Debug && msg != nullptr && std::strstr(msg, "GDALOpen(") != nullptr &&
                std::strstr(msg, "succeeds") != nullptr && std::strstr(msg, "GDALOpen(/vsimem/") == nullptr)
                openCount++;
        }
    }

    fs::path createGeoRaster(const fs::path &path, const GeoRasterSpec &spec)
    {
        GDALDriverH drv = GDALGetDriverByName("GTiff");
        char **opts = CSLSetNameValue(nullptr, "TILED", "YES");
        GDALDatasetH ds = GDALCreate(drv, path.string().c_str(), spec.width, spec.height, spec.bands, spec.type, opts);
        CSLDestroy(opts);
        if (ds == nullptr)
            throw std::runtime_error("Cannot create " + path.string());

        double gt[6] = {spec.originX, spec.pixelSize, 0.0, spec.originY, 0.0, -spec.pixelSize};
        GDALSetGeoTransform(ds, gt);
        OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(srs, spec.epsg);
        char *wkt = nullptr;
        OSRExportToWkt(srs, &wkt);
        GDALSetProjection(ds, wkt);
        CPLFree(wkt);
        OSRDestroySpatialReference(srs);

        std::vector<double> row(spec.width);
        for (int b = 1; b <= spec.bands; b++)
        {
            GDALRasterBandH band = GDALGetRasterBand(ds, b);
            const bool alpha = spec.alphaBand && b == spec.bands;
            if (alpha)
                GDALSetRasterColorInterpretation(band, GCI_AlphaBand);
            for (int y = 0; y < spec.height; y++)
            {
                for (int x = 0; x < spec.width; x++)
                    row[x] = alpha ? 255.0 : spec.value(b, x, y);
                if (GDALRasterIO(band, GF_Write, 0, y, spec.width, 1, row.data(), spec.width, 1, GDT_Float64, 0, 0) !=
                    CE_None)
                    throw std::runtime_error("Cannot write " + path.string());
            }
        }
        GDALClose(ds);
        return path;
    }

    DecodedImage decodeImage(const uint8_t *data, size_t size)
    {
        const std::string vsi = "/vsimem/test-decode-" + ddb::utils::generateRandomString(16);
        VSIFCloseL(VSIFileFromMemBuffer(vsi.c_str(), const_cast<uint8_t *>(data), size, FALSE));
        GDALDatasetH ds = GDALOpen(vsi.c_str(), GA_ReadOnly);
        if (ds == nullptr)
        {
            VSIUnlink(vsi.c_str());
            throw std::runtime_error("Cannot decode image");
        }

        DecodedImage img;
        img.width = GDALGetRasterXSize(ds);
        img.height = GDALGetRasterYSize(ds);
        for (int b = 1; b <= GDALGetRasterCount(ds); b++)
        {
            std::vector<uint8_t> plane(static_cast<size_t>(img.width) * img.height);
            GDALRasterBandH band = GDALGetRasterBand(ds, b);
            if (GDALRasterIO(band, GF_Read, 0, 0, img.width, img.height, plane.data(), img.width, img.height,
                             GDT_Byte, 0, 0) != CE_None)
                throw std::runtime_error("Cannot read decoded band");
            img.bands.push_back(std::move(plane));
            img.colorInterp.push_back(GDALGetRasterColorInterpretation(band));
        }
        GDALClose(ds);
        VSIUnlink(vsi.c_str());
        return img;
    }

    DecodedImage decodeImage(const std::vector<uint8_t> &data)
    {
        return decodeImage(data.data(), data.size());
    }

    DecodedImage decodeImageFile(const fs::path &path)
    {
        VSILFILE *f = VSIFOpenL(path.string().c_str(), "rb");
        if (f == nullptr)
            throw std::runtime_error("Cannot open " + path.string());
        std::vector<uint8_t> data;
        uint8_t chunk[65536];
        size_t n;
        while ((n = VSIFReadL(chunk, 1, sizeof(chunk), f)) > 0)
            data.insert(data.end(), chunk, chunk + n);
        VSIFCloseL(f);
        return decodeImage(data);
    }

    GdalOpenCounter::GdalOpenCounter()
    {
        const char *debug = CPLGetConfigOption("CPL_DEBUG", nullptr);
        previousDebug_ = debug != nullptr ? debug : "";
        openCount = 0;
        CPLSetConfigOption("CPL_DEBUG", "ON");
        previous_ = CPLSetErrorHandler(countOpens);
    }

    GdalOpenCounter::~GdalOpenCounter()
    {
        CPLSetErrorHandler(previous_);
        CPLSetConfigOption("CPL_DEBUG", previousDebug_.empty() ? nullptr : previousDebug_.c_str());
    }

    int GdalOpenCounter::count() const
    {
        return openCount.load();
    }

    std::vector<uint8_t> takeBuffer(uint8_t *buffer, int size)
    {
        std::vector<uint8_t> out(buffer, buffer + size);
        VSIFree(buffer);
        return out;
    }
}
