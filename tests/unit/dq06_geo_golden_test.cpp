/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-06 golden outputs of the geographic code: the DEM analyses (volume, stockpile, profile,
// contour) on the same terrain in five coordinate reference systems, the coordinate
// transformations, the raster footprints and the vector extents. The goldens were taken from
// the code before DQ-06 and pin it: the refactor of the transforms, the DEM setup and the
// nodata rules must not change any of them.

#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "contour.h"
#include "coordstransformer.h"
#include "ddb.h"
#include "dq06_golden_support.h"
#include "entry.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "geo.h"
#include "raster_profile.h"
#include "stockpile.h"
#include "thermal.h"
#include "volume.h"

namespace
{
    using namespace ddb;

    constexpr double kNodata = -9999.0;

    // The terrain of every DEM: a plane, a conical mound centred on pixel (35, 35) and a
    // nodata patch. Only + - * / and sqrt, which are exact IEEE operations, so every platform
    // writes the same floats.
    float terrain(int x, int y)
    {
        if (x >= 10 && x < 15 && y >= 50 && y < 55)
            return static_cast<float>(kNodata);
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
    const std::vector<DemSpec> &demSpecs()
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

    std::string wktOf(int epsg)
    {
        OGRSpatialReference srs;
        srs.importFromEPSG(epsg);
        char *wkt = nullptr;
        srs.exportToWkt(&wkt);
        std::string out = wkt;
        CPLFree(wkt);
        return out;
    }

    fs::path makeDem(const fs::path &path, const DemSpec &spec, std::optional<double> nodata = kNodata)
    {
        GDALDriver *drv = GetGDALDriverManager()->GetDriverByName("GTiff");
        char **opts = CSLSetNameValue(nullptr, "COMPRESS", "LZW");
        std::unique_ptr<GDALDataset> ds(drv->Create(path.string().c_str(), spec.size, spec.size, 1, GDT_Float32, opts));
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
                row[x] = terrain(x, y);
            band->RasterIO(GF_Write, 0, y, spec.size, 1, row.data(), spec.size, 1, GDT_Float32, 0, 0);
        }
        if (nodata)
            band->SetNoDataValue(*nodata);
        ds->FlushCache();
        return path;
    }

    // Longitude and latitude of a pixel corner, with the traditional axis order set
    // explicitly (the test does not depend on the process-wide setting)
    std::pair<double, double> lonLatOfPixel(const DemSpec &spec, double px, double py)
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

    json lineString(const DemSpec &spec, std::vector<std::pair<double, double>> pixels)
    {
        json coords = json::array();
        for (const auto &[px, py] : pixels)
        {
            const auto [lon, lat] = lonLatOfPixel(spec, px, py);
            coords.push_back({lon, lat});
        }
        return {{"type", "LineString"}, {"coordinates", coords}};
    }

    json polygon(const DemSpec &spec, std::vector<std::pair<double, double>> pixels)
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

    class Dq06Geo : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    TEST_F(Dq06Geo, profileOnEveryCrs)
    {
        TestArea ta(TEST_NAME, true);
        for (const auto &spec : demSpecs())
        {
            SCOPED_TRACE(spec.name);
            const fs::path dem = makeDem(ta.getPath(spec.name + ".tif"), spec);
            // Across the mound, then over the nodata patch
            const json across = lineString(spec, {{5.5, 35.5}, {35.5, 35.5}, {65.5, 40.5}});
            dq06::expectGolden("dem_" + spec.name + "_profile",
                               dq06::capture([&] { return getRasterProfileJson(dem.string(), across.dump(), 25); }));
            const json overNodata = lineString(spec, {{12.5, 45.5}, {12.5, 60.5}});
            dq06::expectGolden("dem_" + spec.name + "_profile_nodata",
                               dq06::capture([&] { return getRasterProfileJson(dem.string(), overNodata.dump(), 31); }));
        }
    }

    TEST_F(Dq06Geo, volumeOnEveryCrs)
    {
        TestArea ta(TEST_NAME, true);
        for (const auto &spec : demSpecs())
        {
            SCOPED_TRACE(spec.name);
            const fs::path dem = makeDem(ta.getPath(spec.name + ".tif"), spec);
            const json pile = polygon(spec, {{12, 12}, {58, 12}, {58, 58}, {12, 58}});
            for (const char *method : {"lowest_perimeter", "average_perimeter", "best_fit", "flat"})
            {
                dq06::expectGolden("dem_" + spec.name + "_volume_" + method, dq06::capture([&] {
                                       return calculateVolumeJson(dem.string(), pile.dump(), method, 100.5);
                                   }));
            }
            // A polygon over the nodata patch exercises the nodata rule
            const json holey = polygon(spec, {{6, 44}, {20, 44}, {20, 64}, {6, 64}});
            dq06::expectGolden("dem_" + spec.name + "_volume_nodata", dq06::capture([&] {
                                   return calculateVolumeJson(dem.string(), holey.dump(), "lowest_perimeter", 0.0);
                               }));
        }
    }

    TEST_F(Dq06Geo, stockpileOnEveryCrs)
    {
        TestArea ta(TEST_NAME, true);
        for (const auto &spec : demSpecs())
        {
            SCOPED_TRACE(spec.name);
            const fs::path dem = makeDem(ta.getPath(spec.name + ".tif"), spec);
            const auto [lon, lat] = lonLatOfPixel(spec, 35.5, 35.5);
            dq06::expectGolden("dem_" + spec.name + "_stockpile", dq06::capture([&] {
                                   return detectStockpileJson(dem.string(), lat, lon, 300.0, 0.5f);
                               }));
            dq06::expectGolden("dem_" + spec.name + "_stockpile_sensitive", dq06::capture([&] {
                                   return detectStockpileJson(dem.string(), lat, lon, 450.0, 0.9f);
                               }));
            dq06::expectGolden("dem_" + spec.name + "_stockpile_all", dq06::capture([&] {
                                   return detectAllStockpilesJson(dem.string(), 0.5f, 0.0, 10);
                               }));
        }
    }

    TEST_F(Dq06Geo, contourOnEveryCrs)
    {
        TestArea ta(TEST_NAME, true);
        for (const auto &spec : demSpecs())
        {
            SCOPED_TRACE(spec.name);
            const fs::path dem = makeDem(ta.getPath(spec.name + ".tif"), spec);
            ContourOptions interval;
            interval.interval = 1.0;
            dq06::expectGolden("dem_" + spec.name + "_contour_interval",
                               dq06::capture([&] { return generateContoursJson(dem.string(), interval); }));
            ContourOptions count;
            count.count = 6;
            count.baseOffset = 0.5;
            count.simplifyTolerance = 0.0;
            dq06::expectGolden("dem_" + spec.name + "_contour_count",
                               dq06::capture([&] { return generateContoursJson(dem.string(), count); }));
        }
    }

    TEST_F(Dq06Geo, demErrors)
    {
        TestArea ta(TEST_NAME, true);
        const std::string dir = ta.getFolder().string();
        const std::string missing = ta.getPath("missing.tif").string();
        const json line = {{"type", "LineString"}, {"coordinates", {{10.0, 45.0}, {10.001, 45.0}}}};
        const json pile = {{"type", "Polygon"},
                           {"coordinates", {{{10.0, 45.0}, {10.001, 45.0}, {10.001, 44.999}, {10.0, 45.0}}}}};
        dq06::expectGolden("dem_missing_profile",
                           dq06::capture([&] { return getRasterProfileJson(missing, line.dump(), 10); }, dir));
        dq06::expectGolden("dem_missing_volume", dq06::capture([&] {
                               return calculateVolumeJson(missing, pile.dump(), "flat", 1.0);
                           }, dir));
        dq06::expectGolden("dem_missing_stockpile",
                           dq06::capture([&] { return detectStockpileJson(missing, 45.0, 10.0, 50.0, 0.5f); }, dir));
        dq06::expectGolden("dem_missing_stockpile_all",
                           dq06::capture([&] { return detectAllStockpilesJson(missing, 0.5f, 0.0, 5); }, dir));

        // A raster without a geotransform
        const fs::path plain = ta.getPath("plain.tif");
        {
            GDALDriver *drv = GetGDALDriverManager()->GetDriverByName("GTiff");
            std::unique_ptr<GDALDataset> ds(drv->Create(plain.string().c_str(), 8, 8, 1, GDT_Float32, nullptr));
            ASSERT_NE(ds, nullptr);
        }
        dq06::expectGolden("dem_plain_profile",
                           dq06::capture([&] { return getRasterProfileJson(plain.string(), line.dump(), 10); }, dir));
        dq06::expectGolden("dem_plain_volume", dq06::capture([&] {
                               return calculateVolumeJson(plain.string(), pile.dump(), "flat", 1.0);
                           }, dir));
        dq06::expectGolden("dem_plain_stockpile",
                           dq06::capture([&] { return detectStockpileJson(plain.string(), 45.0, 10.0, 50.0, 0.5f); }, dir));
        ContourOptions options;
        options.interval = 1.0;
        dq06::expectGolden("dem_plain_contour",
                           dq06::capture([&] { return generateContoursJson(plain.string(), options); }, dir));
    }

    // A DEM with no CRS: map coordinates are taken as WGS84
    TEST_F(Dq06Geo, demWithoutCrs)
    {
        TestArea ta(TEST_NAME, true);
        const DemSpec spec{"nocrs", 4326, 10.0, 45.0, 0.0001};
        const fs::path dem = ta.getPath("nocrs.tif");
        {
            GDALDriver *drv = GetGDALDriverManager()->GetDriverByName("GTiff");
            std::unique_ptr<GDALDataset> ds(drv->Create(dem.string().c_str(), 70, 70, 1, GDT_Float32, nullptr));
            double gt[6] = {10.0, 0.0001, 0.0, 45.0, 0.0, -0.0001};
            ds->SetGeoTransform(gt);
            std::vector<float> row(70);
            for (int y = 0; y < 70; y++)
            {
                for (int x = 0; x < 70; x++)
                    row[x] = terrain(x, y);
                ds->GetRasterBand(1)->RasterIO(GF_Write, 0, y, 70, 1, row.data(), 70, 1, GDT_Float32, 0, 0);
            }
            ds->GetRasterBand(1)->SetNoDataValue(kNodata);
        }
        const json across = lineString(spec, {{5.5, 35.5}, {65.5, 40.5}});
        dq06::expectGolden("dem_nocrs_profile",
                           dq06::capture([&] { return getRasterProfileJson(dem.string(), across.dump(), 20); }));
        const json pile = polygon(spec, {{12, 12}, {58, 12}, {58, 58}, {12, 58}});
        dq06::expectGolden("dem_nocrs_volume", dq06::capture([&] {
                               return calculateVolumeJson(dem.string(), pile.dump(), "lowest_perimeter", 0.0);
                           }));
        const auto [lon, lat] = lonLatOfPixel(spec, 35.5, 35.5);
        dq06::expectGolden("dem_nocrs_stockpile", dq06::capture([&] {
                               return detectStockpileJson(dem.string(), lat, lon, 300.0, 0.5f);
                           }));
        dq06::expectGolden("dem_nocrs_stockpile_all",
                           dq06::capture([&] { return detectAllStockpilesJson(dem.string(), 0.5f, 0.0, 10); }));
        ContourOptions options;
        options.interval = 2.0;
        dq06::expectGolden("dem_nocrs_contour",
                           dq06::capture([&] { return generateContoursJson(dem.string(), options); }));
    }

    // ---- coordinate transformations ----------------------------------------------------

    json transformed(CoordsTransformer &t, const std::vector<std::pair<double, double>> &points)
    {
        json out = json::array();
        for (auto [x, y] : points)
        {
            t.transform(&x, &y);
            out.push_back({x, y});
        }
        return out;
    }

    TEST_F(Dq06Geo, coordsTransformerKeepsTheTraditionalAxisOrder)
    {
        const std::vector<std::pair<double, double>> lonLat = {
            {10.0, 45.0}, {-91.99321949277439, 46.842979268105516}, {175.40, -41.07}, {0.0, 0.0}};
        json out = json::object();
        for (const int epsg : {3857, 4258, 3035, 32632, 32719, 2056, 4979})
        {
            CoordsTransformer t(4326, epsg);
            out["from4326_to" + std::to_string(epsg)] = transformed(t, lonLat);
        }
        {
            CoordsTransformer t(4258, 4326);
            out["from4258_to4326"] = transformed(t, lonLat);
        }
        {
            CoordsTransformer t(3035, 4326);
            out["from3035_to4326"] = transformed(t, {{4300000.0, 2500000.0}, {4321000.0, 3210000.0}});
        }
        {
            CoordsTransformer t(wktOf(32632), 4326);
            out["fromWkt32632_to4326"] = transformed(t, {{578000.0, 4985000.0}, {500000.0, 0.0}});
        }
        {
            CoordsTransformer t(wktOf(3035), 3857);
            out["fromWkt3035_to3857"] = transformed(t, {{4300000.0, 2500000.0}});
        }
        {
            CoordsTransformer t(4326, wktOf(4258));
            out["from4326_toWkt4258"] = transformed(t, lonLat);
        }
        {
            CoordsTransformer t(4326, wktOf(3035));
            out["from4326_toWkt3035"] = transformed(t, lonLat);
        }
        {
            CoordsTransformer t(4326, 3035);
            std::vector<double> xs = {10.0, 11.0, 12.0}, ys = {45.0, 46.0, 47.0};
            t.transform(xs.size(), xs.data(), ys.data());
            out["batch4326_to3035"] = {{xs[0], ys[0]}, {xs[1], ys[1]}, {xs[2], ys[2]}};
        }
        {
            // Geocentric (ECEF) to WGS84 with a height: tileset roots and positions
            CoordsTransformer t(4978, 4326);
            json ecef = json::array();
            for (const auto &p : std::vector<std::array<double, 3>>{{4396000.0, 722000.0, 4558000.0}, {6378137.0, 0.0, 0.0}})
            {
                double x = p[0], y = p[1], z = p[2];
                t.transform(&x, &y, &z);
                ecef.push_back({x, y, z});
            }
            out["from4978_to4326"] = ecef;
        }
        dq06::expectGolden("coords_transformer", out);

        // The axis order, whatever the global setting: x is the longitude or the easting
        CoordsTransformer geographic(4326, 4258);
        double x = 10.0, y = 45.0;
        geographic.transform(&x, &y);
        EXPECT_NEAR(x, 10.0, 1e-6);
        EXPECT_NEAR(y, 45.0, 1e-6);
        CoordsTransformer laea(4326, 3035);
        x = 10.0;
        y = 45.0;
        laea.transform(&x, &y);
        EXPECT_GT(x, 4000000.0) << "EPSG:3035 comes out northing first";
        EXPECT_LT(y, 3000000.0);
    }

    TEST_F(Dq06Geo, utmConversions)
    {
        json out = json::array();
        const std::vector<std::pair<double, double>> points = {
            {46.842979268105516, -91.99321949277439}, {-33.8688, 151.2093}, {45.0, 10.0},
            {60.0, 5.0},   {78.2, 15.6},   {0.0, 0.0},   {-45.5, -72.3},  {35.0, 139.7}};
        for (const auto &[lat, lon] : points)
        {
            const UTMZone zone = getUTMZone(lat, lon);
            const Projected2D p = toUTM(lat, lon, zone);
            const Geographic2D g = fromUTM(p, zone);
            out.push_back({{"zone", zone.zone}, {"north", zone.north}, {"x", p.x}, {"y", p.y},
                           {"lat", g.latitude}, {"lon", g.longitude}});
        }
        dq06::expectGolden("utm_conversions", out);
    }

    TEST_F(Dq06Geo, pixelToGeoOnEveryCrs)
    {
        json out = json::object();
        for (const auto &spec : demSpecs())
        {
            const double gt[6] = {spec.originX, spec.pixel, 0.0, spec.originY, 0.0, -spec.pixel};
            double lon = 0, lat = 0;
            pixelToGeo(35.5, 35.5, gt, wktOf(spec.epsg), lon, lat);
            out[spec.name] = {lon, lat};
        }
        // No projection: the map coordinates come back unchanged
        const double gt[6] = {100.0, 2.0, 0.0, 200.0, 0.0, -2.0};
        double x = 0, y = 0;
        pixelToGeo(3.0, 4.0, gt, "", x, y);
        out["noProjection"] = {x, y};
        dq06::expectGolden("pixel_to_geo", out);
    }

    // ---- raster footprints ------------------------------------------------------------

    TEST_F(Dq06Geo, rasterEntryFootprintOnEveryCrs)
    {
        TestArea ta(TEST_NAME, true);
        json out = json::object();
        for (const auto &spec : demSpecs())
        {
            const fs::path dem = makeDem(ta.getPath(spec.name + ".tif"), spec);
            Entry e;
            parseEntry(dem, ta.getFolder(), e, false);
            out[spec.name] = {{"type", static_cast<int>(e.type)},
                              {"point", e.point_geom.toWkt()},
                              {"polygon", e.polygon_geom.toWkt()},
                              {"properties", e.properties}};
        }
        dq06::expectGolden("raster_entry_footprints", out);
    }
}
