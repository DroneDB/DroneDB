/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_test_utils.h"
#include "randomutil.h"

#include <cstring>
#include <mutex>
#include <stdexcept>

#include "utils.h"
#include "mio.h"

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
        GDALDatasetH ds = GDALCreate(drv, ddb::io::toUtf8(path).c_str(), spec.width, spec.height, spec.bands, spec.type, opts);
        CSLDestroy(opts);
        if (ds == nullptr)
            throw std::runtime_error("Cannot create " + ddb::io::toUtf8(path));

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
            if (b <= static_cast<int>(spec.bandNames.size()))
                GDALSetDescription(band, spec.bandNames[b - 1].c_str());
            for (int y = 0; y < spec.height; y++)
            {
                for (int x = 0; x < spec.width; x++)
                    row[x] = alpha ? spec.alphaValue(x, y) : spec.value(b, x, y);
                if (GDALRasterIO(band, GF_Write, 0, y, spec.width, 1, row.data(), spec.width, 1, GDT_Float64, 0, 0) !=
                    CE_None)
                    throw std::runtime_error("Cannot write " + ddb::io::toUtf8(path));
            }
        }
        GDALClose(ds);
        return path;
    }

    fs::path createGeoJp2(const fs::path &path, const GeoRasterSpec &spec, bool georeferenced)
    {
        const fs::path source = path.string() + ".source.tif";
        createGeoRaster(source, spec);
        GDALDatasetH src = GDALOpen(ddb::io::toUtf8(source).c_str(), GA_ReadOnly);
        if (src == nullptr)
            throw std::runtime_error("Cannot open " + ddb::io::toUtf8(source));
        char **opts = CSLSetNameValue(nullptr, "REVERSIBLE", "YES");
        opts = CSLSetNameValue(opts, "QUALITY", "100");
        if (!georeferenced)
        {
            opts = CSLSetNameValue(opts, "GeoJP2", "NO");
            opts = CSLSetNameValue(opts, "GMLJP2", "NO");
        }
        GDALDatasetH out = GDALCreateCopy(GDALGetDriverByName("JP2OpenJPEG"), ddb::io::toUtf8(path).c_str(), src,
                                          FALSE, opts, nullptr, nullptr);
        CSLDestroy(opts);
        GDALClose(src);
        fs::remove(source);
        if (out == nullptr)
            throw std::runtime_error("Cannot create " + ddb::io::toUtf8(path));
        GDALClose(out);
        return path;
    }

    DecodedImage decodeImage(const uint8_t *data, size_t size)
    {
        const std::string vsi = "/vsimem/test-decode-" + ddb::randomString(16);
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
        VSILFILE *f = VSIFOpenL(ddb::io::toUtf8(path).c_str(), "rb");
        if (f == nullptr)
            throw std::runtime_error("Cannot open " + ddb::io::toUtf8(path));
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
