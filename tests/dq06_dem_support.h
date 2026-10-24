/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DQ06_DEM_SUPPORT_H
#define DQ06_DEM_SUPPORT_H

// The synthetic DEMs of the DQ-06 tests: one terrain (a plane, a conical mound, a nodata patch)
// in five coordinate reference systems, and the lines and polygons over it in WGS84.

#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fs.h"
#include "gdal_inc.h"
#include "json.h"

namespace dq06
{
    // A raster read or write that must work: the fixtures fail loudly instead of carrying on
    // with a half-written file
    inline void checkIo(CPLErr err)
    {
        if (err != CE_None)
            throw std::runtime_error("GDAL raster I/O failed");
    }

    constexpr double kNodata = -9999.0;

    // The terrain of every DEM: a plane, a conical mound centred on pixel (35, 35) and a
    // nodata patch. Only + - * / and sqrt, which are exact IEEE operations, so every platform
    // writes the same floats.
    inline float terrain(int x, int y, float patch = static_cast<float>(kNodata))
    {
        if (x >= 10 && x < 15 && y >= 50 && y < 55)
            return patch;
        const double dx = x - 35.0, dy = y - 35.0;
        const double d = std::sqrt(dx * dx + dy * dy);
        double z = 100.0 + 0.02 * x + 0.01 * y + ((x * 7 + y * 13) % 11) * 0.01;
        if (d < 20.0)
            z += (20.0 - d) * 0.3;
        return static_cast<float>(z);
    }

    struct DemSpec
    {
        std::string name;
        int epsg;
        double originX; // upper-left corner, in the CRS
        double originY;
        double pixel;
        int size = 70;
    };

    // The same terrain in five CRSs, among them the ones whose authority axis order is not
    // x = east: the geographic EPSG:4326 and EPSG:4258 (latitude first) and the projected
    // EPSG:3035 (northing first)
    inline const std::vector<DemSpec> &demSpecs()
    {
        static const std::vector<DemSpec> specs = {
            {"wgs84", 4326, 10.0, 45.0, 0.0001},
            {"etrs89", 4258, 10.0, 45.0, 0.0001},
            {"utm32", 32632, 578000.0, 4985000.0, 10.0},
            {"web", 3857, 1113000.0, 5621000.0, 12.0},
            {"laea", 3035, 4300000.0, 2500000.0, 10.0},
        };
        return specs;
    }

    inline std::string wktOf(int epsg)
    {
        OGRSpatialReference srs;
        srs.importFromEPSG(epsg);
        char *wkt = nullptr;
        srs.exportToWkt(&wkt);
        std::string out = wkt;
        CPLFree(wkt);
        return out;
    }

    inline fs::path makeDem(const fs::path &path, const DemSpec &spec, std::optional<double> nodata = kNodata,
                         float patch = static_cast<float>(kNodata), GDALDataType type = GDT_Float32)
    {
        GDALDriver *drv = GetGDALDriverManager()->GetDriverByName("GTiff");
        char **opts = CSLSetNameValue(nullptr, "COMPRESS", "LZW");
        std::unique_ptr<GDALDataset> ds(drv->Create(path.string().c_str(), spec.size, spec.size, 1, type, opts));
        CSLDestroy(opts);
        if (!ds)
            throw std::runtime_error("Cannot create " + path.string());
        double gt[6] = {spec.originX, spec.pixel, 0.0, spec.originY, 0.0, -spec.pixel};
        ds->SetGeoTransform(gt);
        ds->SetProjection(wktOf(spec.epsg).c_str());
        std::vector<float> row(spec.size);
        GDALRasterBand *band = ds->GetRasterBand(1);
        for (int y = 0; y < spec.size; y++)
        {
            for (int x = 0; x < spec.size; x++)
                row[x] = terrain(x, y, patch);
            dq06::checkIo(band->RasterIO(GF_Write, 0, y, spec.size, 1, row.data(), spec.size, 1, GDT_Float32, 0, 0));
        }
        if (nodata)
            band->SetNoDataValue(*nodata);
        ds->FlushCache();
        return path;
    }

    // Longitude and latitude of a pixel corner, with the traditional axis order set
    // explicitly (the test does not depend on the process-wide setting)
    inline std::pair<double, double> lonLatOfPixel(const DemSpec &spec, double px, double py)
    {
        OGRSpatialReference from, wgs84;
        from.importFromEPSG(spec.epsg);
        wgs84.importFromEPSG(4326);
        from.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&from, &wgs84));
        double x = spec.originX + px * spec.pixel;
        double y = spec.originY - py * spec.pixel;
        if (!ct || !ct->Transform(1, &x, &y))
            throw std::runtime_error("Cannot transform a test point");
        return {x, y};
    }

    inline json lineString(const DemSpec &spec, std::vector<std::pair<double, double>> pixels)
    {
        json coords = json::array();
        for (const auto &[px, py] : pixels)
        {
            const auto [lon, lat] = lonLatOfPixel(spec, px, py);
            coords.push_back({lon, lat});
        }
        return {{"type", "LineString"}, {"coordinates", coords}};
    }

    inline json polygon(const DemSpec &spec, std::vector<std::pair<double, double>> pixels)
    {
        json ring = json::array();
        for (const auto &[px, py] : pixels)
        {
            const auto [lon, lat] = lonLatOfPixel(spec, px, py);
            ring.push_back({lon, lat});
        }
        ring.push_back(ring.front());
        return {{"type", "Polygon"}, {"coordinates", json::array({ring})}};
    }
}

#endif // DQ06_DEM_SUPPORT_H
