/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// One band-value model and sampling at map coordinates (U-201: FR-RAS-02, FR-RAS-04, FR-RAS-07, FR-RAS-21,
// FR-RAS-31, FR-XC-24; D-021, D-023, D-041). The expected numbers are those of gdallocationinfo -wgs84 and
// gdalinfo -stats on the same files (written here with literals), never computed by the code under test.

#include <cmath>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "dem.h"
#include "ddb.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "json.h"
#include "raster_analysis.h"
#include "raster_profile.h"
#include "contour.h"
#include "cog.h"
#include "gdaltiler.h"
#include "raster_stats.h"
#include "tilerhelper.h"
#include "raster_test_utils.h"
#include "raster_utils.h"
#include "thermal.h"
#include "vis_render.h"

namespace
{
    using namespace ddb;

    struct ValueRasterSpec
    {
        int epsg = 32633;
        double originX = 500000.0;
        double originY = 5000000.0;
        double pixel = 2.0;
        int width = 100;
        int height = 100;
        GDALDataType type = GDT_UInt16;
        bool hasScale = false;
        double scale = 1.0;
        double offset = 0.0;
        std::string unit;
        std::optional<double> nodata;
        std::function<double(int x, int y)> value = [](int x, int y) { return 115.0 + x + 2.0 * y; };
    };

    // A one-band GeoTIFF: the pixel (x, y) holds 115 + x + 2y unless the spec says otherwise
    fs::path makeValueRaster(const fs::path &path, const ValueRasterSpec &spec)
    {
        GDALDriverH drv = GDALGetDriverByName("GTiff");
        GDALDatasetH hDs = GDALCreate(drv, path.string().c_str(), spec.width, spec.height, 1, spec.type, nullptr);
        if (!hDs)
            throw std::runtime_error("Cannot create the test raster");
        const double gt[6] = {spec.originX, spec.pixel, 0.0, spec.originY, 0.0, -spec.pixel};
        GDALSetGeoTransform(hDs, const_cast<double *>(gt));
        OGRSpatialReference srs;
        srs.importFromEPSG(spec.epsg);
        srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        char *wkt = nullptr;
        srs.exportToWkt(&wkt);
        GDALSetProjection(hDs, wkt);
        CPLFree(wkt);

        GDALRasterBandH band = GDALGetRasterBand(hDs, 1);
        std::vector<double> row(static_cast<size_t>(spec.width));
        for (int y = 0; y < spec.height; y++)
        {
            for (int x = 0; x < spec.width; x++)
                row[static_cast<size_t>(x)] = spec.value(x, y);
            EXPECT_EQ(GDALRasterIO(band, GF_Write, 0, y, spec.width, 1, row.data(), spec.width, 1, GDT_Float64, 0, 0),
                      CE_None);
        }
        if (spec.hasScale)
        {
            GDALSetRasterScale(band, spec.scale);
            GDALSetRasterOffset(band, spec.offset);
        }
        if (!spec.unit.empty())
            GDALSetRasterUnitType(band, spec.unit.c_str());
        if (spec.nodata)
            GDALSetRasterNoDataValue(band, *spec.nodata);
        GDALFlushCache(hDs);
        GDALClose(hDs);
        return path;
    }

    // UInt16, scale 0.1, unit Celsius: 115..412 read as 11.5..41.2 (FR-RAS-04)
    ValueRasterSpec scaledSpec()
    {
        ValueRasterSpec spec;
        spec.hasScale = true;
        spec.scale = 0.1;
        spec.unit = "Celsius";
        return spec;
    }

    // The WGS84 coordinate of the centre of pixel (col, row) of the spec's raster, by GDAL
    std::pair<double, double> wgs84OfPixelCentre(const ValueRasterSpec &spec, int col, int row)
    {
        double x = spec.originX + (col + 0.5) * spec.pixel;
        double y = spec.originY - (row + 0.5) * spec.pixel;
        OGRSpatialReference from, to;
        from.importFromEPSG(spec.epsg);
        to.importFromEPSG(4326);
        from.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        to.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&from, &to));
        ct->Transform(1, &x, &y);
        return {x, y};
    }

    // ---------------------------------------------------------------- the geo-to-pixel helper (FR-RAS-02)

    TEST(GeoPixelMapTest, mapsAMapCoordinateToItsPixel)
    {
        const double gt[6] = {500000.0, 2.0, 0.0, 5000000.0, 0.0, -2.0};
        const GeoPixelMap map(gt);
        double px = 0, py = 0;
        map.toPixel(500101.0, 4999899.0, px, py);
        EXPECT_DOUBLE_EQ(px, 50.5);
        EXPECT_DOUBLE_EQ(py, 50.5);

        int col = 0, row = 0;
        map.toPixelIndex(500101.0, 4999899.0, col, row);
        EXPECT_EQ(col, 50);
        EXPECT_EQ(row, 50);

        // A point left of and above the raster is a negative pixel, never the edge pixel
        map.toPixelIndex(499999.0, 5000001.0, col, row);
        EXPECT_EQ(col, -1);
        EXPECT_EQ(row, -1);
    }

    TEST(GeoPixelMapTest, aSingularGeotransformIsRefused)
    {
        const double gt[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        EXPECT_THROW(GeoPixelMap{gt}, AppException);
    }

    // gdallocationinfo -wgs84 on the same file: (15.0012848916515, 45.152568005955) is pixel (50, 50), raw 265;
    // (15.0000127218998, 45.1534681816711) is pixel (0, 0), raw 115
    TEST(RasterValues, theHelperFindsThePixelGdalLocationInfoFindsInAUtmRaster)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = makeValueRaster(ta.getPath("utm.tif"), scaledSpec());
        const DemContext dem = openDem(raster.string());

        int col = 0, row = 0;
        dem.wgs84ToPixelIndex(15.0012848916515, 45.152568005955, col, row);
        EXPECT_EQ(col, 50);
        EXPECT_EQ(row, 50);
        dem.wgs84ToPixelIndex(15.0000127218998, 45.1534681816711, col, row);
        EXPECT_EQ(col, 0);
        EXPECT_EQ(row, 0);
    }

    TEST(RasterValues, theHelperFindsThePixelOfAWebMercatorRaster)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec = scaledSpec();
        spec.epsg = 3857;
        spec.originX = 1670000.0;
        spec.originY = 5645600.0;
        const auto raster = makeValueRaster(ta.getPath("m3857.tif"), spec);
        const DemContext dem = openDem(raster.string());

        // gdaltransform of (1670101, 5645499), the centre of pixel (50, 50)
        int col = 0, row = 0;
        dem.wgs84ToPixelIndex(15.002772543233, 45.1521038921635, col, row);
        EXPECT_EQ(col, 50);
        EXPECT_EQ(row, 50);
    }

    // Points off the centre of a pixel: 0.1 and 0.9 of the way across it, and 0.1 m outside the top-left corner of
    // the raster. The WGS84 points are gdaltransform -s_srs EPSG:32633 -t_srs EPSG:4326 of the UTM points in the
    // comments; the pixels are those of gdallocationinfo -wgs84 (and -geoloc of the UTM points) on a 100 x 100,
    // 2 m raster at (500000, 5000000) in EPSG:32633
    TEST(RasterValues, theHelperFindsThePixelGdalLocationInfoFindsOffThePixelCentres)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = makeValueRaster(ta.getPath("utm.tif"), scaledSpec());
        const DemContext dem = openDem(raster.string());

        struct Probe
        {
            double lon, lat;
            int col, row;
        };
        const Probe probes[] = {
            {15.0012747144524, 45.1525752074175, 50, 50},  // (500100.2, 4999899.8): 0.1 of pixel (50, 50)
            {15.001295068848, 45.1525608044916, 50, 50},   // (500101.8, 4999898.2): 0.9 of pixel (50, 50)
            {15.0000025443803, 45.1534753830192, 0, 0},    // (500000.2, 4999999.8): 0.1 of pixel (0, 0)
            {15.0000228994167, 45.1534609803222, 0, 0},    // (500001.8, 4999998.2): 0.9 of pixel (0, 0)
            {15.0000012721901, 45.1534762831876, 0, 0},    // (500000.1, 4999999.9): just inside the corner
            {14.9999987278098, 45.1534780835244, -1, -1},  // (499999.9, 5000000.1): off this file
        };
        for (const Probe &p : probes)
        {
            int col = 0, row = 0;
            dem.wgs84ToPixelIndex(p.lon, p.lat, col, row);
            EXPECT_EQ(col, p.col) << p.lon << " " << p.lat;
            EXPECT_EQ(row, p.row) << p.lon << " " << p.lat;
        }

        // gdallocationinfo: "Location is off this file"; the point query refuses it
        EXPECT_THROW(getRasterPointAtJson(raster.string(), 14.9999987278098, 45.1534780835244), NoRasterDataException);
        const json inside = json::parse(getRasterPointAtJson(raster.string(), 15.0000012721901, 45.1534762831876));
        EXPECT_EQ(inside["x"], 0);
        EXPECT_EQ(inside["y"], 0);
    }

    // The same on a 2 m raster at (1670000, 5645600) in EPSG:3857 (gdaltransform -s_srs EPSG:3857 -t_srs EPSG:4326,
    // gdallocationinfo -wgs84)
    TEST(RasterValues, theHelperFindsThePixelGdalLocationInfoFindsOffThePixelCentresOfAWebMercatorRaster)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec = scaledSpec();
        spec.epsg = 3857;
        spec.originX = 1670000.0;
        spec.originY = 5645600.0;
        const auto raster = makeValueRaster(ta.getPath("m3857.tif"), spec);
        const DemContext dem = openDem(raster.string());

        int col = 0, row = 0;
        dem.wgs84ToPixelIndex(15.0027653567107, 45.1521089602937, col, row); // (1670100.2, 5645499.8)
        EXPECT_EQ(col, 50);
        EXPECT_EQ(row, 50);
        dem.wgs84ToPixelIndex(15.0027797297552, 45.1520988240328, col, row); // (1670101.8, 5645498.2)
        EXPECT_EQ(col, 50);
        EXPECT_EQ(row, 50);
        dem.wgs84ToPixelIndex(15.0018643464807, 45.1527443735497, col, row); // (1669999.9, 5645600.1): off
        EXPECT_EQ(col, -1);
        EXPECT_EQ(row, -1);
    }

    // ---------------------------------------------------------------- point (FR-RAS-02, FR-RAS-04)

    // The value under the click is the value of the pixel, in the physical unit: 265 x 0.1 (AC-010, AC-012)
    TEST(RasterValues, aPointOnAUtmRasterIsThePixelUnderIt)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = makeValueRaster(ta.getPath("utm.tif"), scaledSpec());

        const json centre = json::parse(getRasterPointAtJson(raster.string(), 15.0012848916515, 45.152568005955));
        EXPECT_EQ(centre["x"], 50);
        EXPECT_EQ(centre["y"], 50);
        EXPECT_FLOAT_EQ(centre["value"].get<float>(), 26.5f);
        EXPECT_FLOAT_EQ(centre["rawValue"].get<float>(), 265.0f);
        EXPECT_DOUBLE_EQ(centre["lon"].get<double>(), 15.0012848916515);
        EXPECT_DOUBLE_EQ(centre["lat"].get<double>(), 45.152568005955);

        const json corner = json::parse(getRasterPointAtJson(raster.string(), 15.0000127218998, 45.1534681816711));
        EXPECT_EQ(corner["x"], 0);
        EXPECT_FLOAT_EQ(corner["value"].get<float>(), 11.5f);
    }

    // Raw 291 with scale 0.1 is 29.1 (AC-012): the pixel (76, 50)
    TEST(RasterValues, aRawValueOf291WithAScaleOfOneTenthIs29Point1)
    {
        TestArea ta(TEST_NAME, true);
        const ValueRasterSpec spec = scaledSpec();
        const auto raster = makeValueRaster(ta.getPath("utm.tif"), spec);
        const auto [lon, lat] = wgs84OfPixelCentre(spec, 76, 50);

        const json value = json::parse(getRasterPointAtJson(raster.string(), lon, lat));
        EXPECT_EQ(value["x"], 76);
        EXPECT_EQ(value["y"], 50);
        EXPECT_EQ(value["rawValue"].get<double>(), 291.0);
        EXPECT_EQ(value["value"].dump(), "29.1");
    }

    TEST(RasterValues, aPointOnAWebMercatorRasterIsThePixelUnderIt)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec = scaledSpec();
        spec.epsg = 3857;
        spec.originX = 1670000.0;
        spec.originY = 5645600.0;
        const auto raster = makeValueRaster(ta.getPath("m3857.tif"), spec);

        const json value = json::parse(getRasterPointAtJson(raster.string(), 15.002772543233, 45.1521038921635));
        EXPECT_EQ(value["x"], 50);
        EXPECT_EQ(value["y"], 50);
        EXPECT_FLOAT_EQ(value["value"].get<float>(), 26.5f);
    }

    // The point is the pixel under it anywhere in the pixel, not the nearest corner (a 2 m pixel)
    TEST(RasterValues, aPointNearTheEdgeOfAPixelIsStillThatPixel)
    {
        TestArea ta(TEST_NAME, true);
        const ValueRasterSpec spec = scaledSpec();
        const auto raster = makeValueRaster(ta.getPath("utm.tif"), spec);
        // 0.9 m east of the centre of pixel (50, 50), inside it
        OGRSpatialReference from, to;
        from.importFromEPSG(32633);
        to.importFromEPSG(4326);
        from.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        to.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&from, &to));
        double x = 500101.9, y = 4999899.0;
        ct->Transform(1, &x, &y);

        const json value = json::parse(getRasterPointAtJson(raster.string(), x, y));
        EXPECT_EQ(value["x"], 50);
        EXPECT_FLOAT_EQ(value["value"].get<float>(), 26.5f);
    }

    TEST(RasterValues, aPointOutsideTheRasterIsRefused)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = makeValueRaster(ta.getPath("utm.tif"), scaledSpec());
        EXPECT_THROW(getRasterPointAtJson(raster.string(), 16.0, 45.15), NoRasterDataException);
        EXPECT_THROW(getRasterPointAtJson(raster.string(), 14.9999, 45.1534), NoRasterDataException);
    }

    // The pixel of the point by pixel coordinates keeps its meaning, and is scaled too
    TEST(RasterValues, aPointByPixelCoordinatesIsScaledToo)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = makeValueRaster(ta.getPath("utm.tif"), scaledSpec());
        const json value = json::parse(getRasterPointJson(raster.string(), 76, 50));
        EXPECT_EQ(value["value"].dump(), "29.1");
        EXPECT_EQ(value["rawValue"].get<double>(), 291.0);
    }

    // ---------------------------------------------------------------- NoData override (FR-RAS-31)

    TEST(RasterValues, aPointOnTheOverriddenNodataValueHasNoValue)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec;
        spec.value = [](int x, int) { return x < 10 ? 0.0 : 500.0 + x; };
        const auto raster = makeValueRaster(ta.getPath("zero.tif"), spec);
        const auto [lon, lat] = wgs84OfPixelCentre(spec, 5, 50);

        // Without an override 0 is a value
        EXPECT_EQ(json::parse(getRasterPointAtJson(raster.string(), lon, lat))["rawValue"].get<double>(), 0.0);

        RasterValueOptions options;
        options.nodata = 0.0;
        EXPECT_THROW(getRasterPointAtJson(raster.string(), lon, lat, options), NoRasterDataException);
    }

    // ---------------------------------------------------------------- area (FR-RAS-02)

    // The window of pixels 10..19 x 20..29 of a Web Mercator raster; GDAL: raw 165..192, mean 178.5
    TEST(RasterValues, anAreaIsTheSourcePixelsUnderTheRectangle)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec = scaledSpec();
        spec.epsg = 3857;
        spec.originX = 1670000.0;
        spec.originY = 5645600.0;
        const auto raster = makeValueRaster(ta.getPath("m3857.tif"), spec);

        // gdaltransform of the corners (1670020, 5645540) and (1670040, 5645560)
        const json stats = json::parse(getRasterAreaStatsBoxJson(raster.string(), 15.0020449078528, 45.1523636332569,
                                                                 15.0022245709097, 45.1524903357998));
        EXPECT_EQ(stats["bounds"], json({{"x0", 10}, {"y0", 20}, {"x1", 19}, {"y1", 29}}));
        EXPECT_EQ(stats["pixelCount"], 100);
        EXPECT_NEAR(stats["min"].get<double>(), 16.5, 1e-6);
        EXPECT_NEAR(stats["max"].get<double>(), 19.2, 1e-6);
        EXPECT_NEAR(stats["mean"].get<double>(), 17.85, 1e-5);
        EXPECT_EQ(stats["unit"], "Celsius");
    }

    // A rectangle of the map over a UTM raster: its pixels, the rectangle being curved in that CRS
    TEST(RasterValues, anAreaOverAUtmRasterIsItsPixelsToo)
    {
        TestArea ta(TEST_NAME, true);
        const ValueRasterSpec spec = scaledSpec();
        const auto raster = makeValueRaster(ta.getPath("utm.tif"), spec);

        // The centres of pixels (10, 20) and (19, 29), plus 0.4 pixel round them
        const auto sw = wgs84OfPixelCentre(spec, 10, 29);
        const auto ne = wgs84OfPixelCentre(spec, 19, 20);
        const json stats = json::parse(getRasterAreaStatsBoxJson(raster.string(), sw.first - 4e-6, sw.second - 3e-6,
                                                                 ne.first + 4e-6, ne.second + 3e-6));
        EXPECT_EQ(stats["bounds"], json({{"x0", 10}, {"y0", 20}, {"x1", 19}, {"y1", 29}}));
        EXPECT_NEAR(stats["mean"].get<double>(), 17.85, 1e-5);
    }

    TEST(RasterValues, anAreaOutsideTheRasterIsRefused)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = makeValueRaster(ta.getPath("utm.tif"), scaledSpec());
        EXPECT_THROW(getRasterAreaStatsBoxJson(raster.string(), 16.0, 45.0, 16.1, 45.1), NoRasterDataException);
    }

    // With 0 as NoData the statistics leave the 0 background out (FR-RAS-31)
    TEST(RasterValues, anAreaExcludesTheOverriddenNodataValue)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec;
        spec.value = [](int x, int) { return x < 50 ? 0.0 : 100.0; };
        const auto raster = makeValueRaster(ta.getPath("zero.tif"), spec);

        const json all = json::parse(getRasterAreaStatsJson(raster.string(), 0, 0, 99, 99));
        EXPECT_EQ(all["pixelCount"], 10000);
        EXPECT_EQ(all["min"].get<double>(), 0.0);

        RasterValueOptions options;
        options.nodata = 0.0;
        const json valid = json::parse(getRasterAreaStatsJson(raster.string(), 0, 0, 99, 99, options));
        EXPECT_EQ(valid["pixelCount"], 5000);
        EXPECT_EQ(valid["min"].get<double>(), 100.0);
    }

    // ---------------------------------------------------------------- profile (FR-RAS-04)

    // Along the row of pixel 50, from the centre of pixel 10 to that of pixel 20: raw 225..235 (22.5..23.5)
    TEST(RasterValues, aProfileIsInPhysicalValues)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec = scaledSpec();
        spec.epsg = 3857;
        spec.originX = 1670000.0;
        spec.originY = 5645600.0;
        const auto raster = makeValueRaster(ta.getPath("m3857.tif"), spec);
        const auto a = wgs84OfPixelCentre(spec, 10, 50);
        const auto b = wgs84OfPixelCentre(spec, 20, 50);
        const json line = {{"type", "LineString"}, {"coordinates", {{a.first, a.second}, {b.first, b.second}}}};

        const json profile = json::parse(getRasterProfileJson(raster.string(), line.dump(), 11));
        EXPECT_NEAR(profile["min"].get<double>(), 22.5, 1e-4);
        EXPECT_NEAR(profile["max"].get<double>(), 23.5, 1e-4);
        EXPECT_EQ(profile["unit"], "Celsius");
    }

    // ---------------------------------------------------------------- value info and statistics (FR-RAS-07, D-041)

    TEST(RasterValues, theValueInfoIsPhysicalAndExact)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = makeValueRaster(ta.getPath("utm.tif"), scaledSpec());

        const json info = json::parse(getRasterValueInfoJson(raster.string()));
        EXPECT_NEAR(info["valueMin"].get<double>(), 11.5, 1e-4); // 115 x 0.1
        EXPECT_NEAR(info["valueMax"].get<double>(), 41.2, 1e-4); // 412 x 0.1
        EXPECT_EQ(info["unit"], "Celsius");
        EXPECT_EQ(info["pixelCount"], 10000);
        EXPECT_EQ(info["validPixelCount"], 10000);
        EXPECT_EQ(info["nodataInvalid"], false);
        // A single band: min to max (D-041)
        ASSERT_EQ(info["stretch"].size(), 1u);
        EXPECT_NEAR(info["stretch"][0]["min"].get<double>(), 11.5, 1e-4);
        EXPECT_NEAR(info["stretch"][0]["max"].get<double>(), 41.2, 1e-4);
    }

    // A raster with a few high pixels and overviews: the range is that of gdalinfo -stats (exact), not the
    // one of an overview
    TEST(RasterValues, theRangeIsTheExactOneNotTheOneOfAnOverview)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec;
        spec.width = 512;
        spec.height = 512;
        spec.type = GDT_Float32;
        spec.value = [](int x, int y) { return (x == 301 && y == 77) || (x == 5 && y == 400) ? 1000.0 : 1.0; };
        const auto raster = makeValueRaster(ta.getPath("peaks.tif"), spec);
        {
            GDALDatasetH hDs = GDALOpen(raster.string().c_str(), GA_Update);
            int levels[2] = {2, 4};
            EXPECT_EQ(GDALBuildOverviews(hDs, "NEAREST", 2, levels, 0, nullptr, nullptr, nullptr), CE_None);
            GDALClose(hDs);
        }

        const json info = json::parse(getRasterValueInfoJson(raster.string()));
        EXPECT_EQ(info["valueMax"].get<double>(), 1000.0);
        EXPECT_EQ(info["valueMin"].get<double>(), 1.0);
    }

    TEST(RasterValues, theStatisticsExcludeTheOverriddenNodataValue)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec;
        spec.value = [](int x, int y) { return x < 20 ? 0.0 : 100.0 + (y % 10); };
        const auto raster = makeValueRaster(ta.getPath("background.tif"), spec);

        const json plain = json::parse(getRasterValueInfoJson(raster.string()));
        EXPECT_EQ(plain["valueMin"].get<double>(), 0.0);
        EXPECT_EQ(plain["validPixelCount"], 10000);

        RasterValueOptions options;
        options.nodata = 0.0;
        const json info = json::parse(getRasterValueInfoJson(raster.string(), options));
        EXPECT_EQ(info["valueMin"].get<double>(), 100.0);
        EXPECT_EQ(info["valueMax"].get<double>(), 109.0);
        EXPECT_EQ(info["validPixelCount"], 8000);
        EXPECT_EQ(info["nodata"], 0.0);
        EXPECT_EQ(info["nodataOverridden"], true);
    }

    // The NoData value of the band cannot be held by its type: reported, so that the Hub can warn
    TEST(RasterValues, aNodataValueOutsideTheTypeIsReportedInvalid)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec;
        spec.nodata = 65536.0;
        const auto raster = makeValueRaster(ta.getPath("invalid.tif"), spec);
        const json info = json::parse(getRasterValueInfoJson(raster.string()));
        EXPECT_EQ(info["nodataInvalid"], true);

        RasterValueOptions options;
        options.nodata = 0.0;
        EXPECT_EQ(json::parse(getRasterValueInfoJson(raster.string(), options))["nodataInvalid"], false);
    }

    // D-023: the pixels of a window, for the size of an export
    TEST(RasterValues, theValueInfoCountsTheSourcePixelsOfAWindow)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec = scaledSpec();
        spec.epsg = 3857;
        spec.originX = 1670000.0;
        spec.originY = 5645600.0;
        const auto raster = makeValueRaster(ta.getPath("m3857.tif"), spec);

        RasterValueOptions options;
        options.bbox = std::array<double, 4>{15.0020449078528, 45.1523636332569, 15.0022245709097, 45.1524903357998};
        const json info = json::parse(getRasterValueInfoJson(raster.string(), options));
        EXPECT_EQ(info["windowPixelCount"], 100);
        EXPECT_EQ(info["validPixelCount"], 10000);
    }

    // A box with minLon > maxLon runs across the antimeridian: on a raster in Europe it is refused, not swapped into
    // a box round the rest of the Earth (whose window was the whole raster, 10000 pixels)
    TEST(RasterValues, aBoxAcrossTheAntimeridianIsRefusedOnARasterThatDoesNotCrossIt)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = makeValueRaster(ta.getPath("utm.tif"), scaledSpec());

        RasterValueOptions options;
        options.bbox = std::array<double, 4>{15.0022245709097, 45.1523636332569, 15.0020449078528, 45.1524903357998};
        EXPECT_THROW(getRasterValueInfoJson(raster.string(), options), InvalidArgsException);
        EXPECT_THROW(getRasterAreaStatsBoxJson(raster.string(), 15.0022245709097, 45.1523636332569, 15.0020449078528,
                                               45.1524903357998),
                     InvalidArgsException);

        const double bbox[4] = {15.0022245709097, 45.1523636332569, 15.0020449078528, 45.1524903357998};
        DDBRasterOptions cOptions = {};
        cOptions.structSize = sizeof(cOptions);
        cOptions.flags = DDB_RASTER_OPT_BBOX;
        std::copy(bbox, bbox + 4, cOptions.bbox);
        char *out = nullptr;
        EXPECT_EQ(DDBGetRasterValueInfoEx(raster.string().c_str(), &cOptions, &out), DDBERR_INVALIDARGS);
        EXPECT_NE(std::string(DDBGetLastError()).find("antimeridian"), std::string::npos) << DDBGetLastError();
        if (out != nullptr)
            DDBFree(out);
    }

    // A raster across the antimeridian (UTM 60S, Fiji, 230 x 30 pixels of 100 m from (808000, 8119000)): the box from
    // 179.95 E to 179.95 W is the window between them. gdallocationinfo -wgs84 of its corners (179.95, -17.01),
    // (179.95, -17.0), (-179.95, -17.01), (-179.95, -17.0): pixels (61, 20), (61, 9), (167, 21), (167, 10), so
    // columns 61..167 and rows 9..21, 107 x 13 pixels
    TEST(RasterValues, aBoxAcrossTheAntimeridianIsTheWindowOfARasterThatCrossesIt)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec;
        spec.epsg = 32760;
        spec.originX = 808000.0;
        spec.originY = 8119000.0;
        spec.pixel = 100.0;
        spec.width = 230;
        spec.height = 30;
        const auto raster = makeValueRaster(ta.getPath("fiji.tif"), spec);

        RasterValueOptions options;
        options.bbox = std::array<double, 4>{179.95, -17.01, -179.95, -17.0};
        const json info = json::parse(getRasterValueInfoJson(raster.string(), options));
        EXPECT_EQ(info["windowPixelCount"], 1391);
        EXPECT_EQ(info["pixelCount"], 6900);

        const json stats = json::parse(getRasterAreaStatsBoxJson(raster.string(), 179.95, -17.01, -179.95, -17.0));
        EXPECT_EQ(stats["bounds"], json({{"x0", 61}, {"y0", 9}, {"x1", 167}, {"y1", 21}}));
    }

    // An approximate sidecar of an earlier build is ignored and replaced by the exact entry
    TEST(RasterStats, anApproximateSidecarIsReplacedByTheExactOne)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec;
        spec.type = GDT_Float32;
        spec.value = [](int x, int) { return static_cast<double>(x); };
        const auto raster = makeValueRaster(ta.getPath("ramp.tif"), spec);
        // The sidecar of a build before the exact statistics: no "exact" member, a wrong maximum
        {
            std::ofstream out(statsSidecarOf(raster));
            out << R"({"bands":{"1":{"min":0,"max":5,"mean":2,"std":1,"p2":0,"p98":5}}})";
        }
        BandStatistics ignored;
        EXPECT_FALSE(readStatsSidecar(raster.string(), 1, ignored));

        RasterStatsProvider::instance().clear();
        GDALDatasetH hDs = GDALOpen(raster.string().c_str(), GA_ReadOnly);
        ASSERT_NE(hDs, nullptr);
        const BandStatistics stats = RasterStatsProvider::instance().get(raster.string(), hDs, 1, std::nullopt, true);
        GDALClose(hDs);
        EXPECT_EQ(stats.max, 99.0);
        EXPECT_EQ(stats.min, 0.0);
        EXPECT_EQ(stats.validCount, 10000u);

        BandStatistics stored;
        ASSERT_TRUE(readStatsSidecar(raster.string(), 1, stored));
        EXPECT_EQ(stored.max, 99.0);
    }

    // One entry per key: the statistics with a NoData override do not replace those without
    TEST(RasterStats, theSidecarKeepsOneEntryPerNodataKey)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec;
        spec.value = [](int x, int) { return x < 50 ? 0.0 : 200.0; };
        const auto raster = makeValueRaster(ta.getPath("zero.tif"), spec);

        RasterStatsProvider::instance().clear();
        GDALDatasetH hDs = GDALOpen(raster.string().c_str(), GA_ReadOnly);
        ASSERT_NE(hDs, nullptr);
        auto &provider = RasterStatsProvider::instance();
        const BandStatistics plain = provider.get(raster.string(), hDs, 1, std::nullopt, true);
        const BandStatistics overridden = provider.get(raster.string(), hDs, 1, 0.0, true);
        GDALClose(hDs);
        EXPECT_EQ(plain.min, 0.0);
        EXPECT_EQ(overridden.min, 200.0);
        EXPECT_EQ(overridden.validCount, 5000u);

        BandStatistics fromSidecar;
        ASSERT_TRUE(readStatsSidecar(raster.string(), 1, fromSidecar, 0.0));
        EXPECT_EQ(fromSidecar.min, 200.0);
        ASSERT_TRUE(readStatsSidecar(raster.string(), 1, fromSidecar));
        EXPECT_EQ(fromSidecar.min, 0.0);
    }

    // ---------------------------------------------------------------- the stretch of a 16-bit ortho (FR-RAS-21, D-041)

    TEST(RasterValues, aMultiBandRasterHasOneStretchPerBand)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = 128;
        spec.height = 128;
        spec.bands = 3;
        spec.type = GDT_UInt16;
        spec.value = [](int band, int x, int y) { return band * 1000.0 + (x + y) * 4.0; };
        const auto raster = testutils::createGeoRaster(ta.getPath("ortho16.tif"), spec);

        const json info = json::parse(getRasterValueInfoJson(raster.string()));
        ASSERT_EQ(info["stretch"].size(), 3u);
        // Band 2: 2000 + 4 (x + y) over x + y in 0..254; the mean is 2508 and the standard deviation 4 x 42.4 x ...:
        // clamped to the minimum and the maximum of the band
        for (int b = 0; b < 3; b++)
        {
            const double lo = info["stretch"][b]["min"].get<double>();
            const double hi = info["stretch"][b]["max"].get<double>();
            EXPECT_GE(lo, (b + 1) * 1000.0);
            EXPECT_LE(hi, (b + 1) * 1000.0 + 1016.0);
            EXPECT_LT(lo, hi);
        }
        // Each band has its own range, not one range for all the bands
        EXPECT_NE(info["stretch"][0]["min"], info["stretch"][2]["min"]);
    }

    // A Byte RGB raster has no stretch
    TEST(RasterValues, aByteRgbRasterHasNoStretch)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = 64;
        spec.height = 64;
        spec.value = [](int, int x, int) { return static_cast<double>(x); };
        const auto raster = testutils::createGeoRaster(ta.getPath("rgb.tif"), spec);
        const json info = json::parse(getRasterValueInfoJson(raster.string()));
        EXPECT_TRUE(info["stretch"].empty());
    }

    // ---------------------------------------------------------------- the C ABI

    TEST(RasterValuesCApi, theExportsTakeMapCoordinatesAndOptions)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = makeValueRaster(ta.getPath("utm.tif"), scaledSpec());
        const std::string path = raster.string();

        char *output = nullptr;
        ASSERT_EQ(DDBGetRasterPointValueEx(path.c_str(), 15.0012848916515, 45.152568005955, nullptr, &output), DDBERR_NONE);
        EXPECT_FLOAT_EQ(json::parse(output)["value"].get<float>(), 26.5f);
        DDBFree(output);

        DDBRasterOptions options = {};
        options.structSize = sizeof(options);
        options.flags = DDB_RASTER_OPT_NODATA;
        options.nodata = 265.0;
        EXPECT_EQ(DDBGetRasterPointValueEx(path.c_str(), 15.0012848916515, 45.152568005955, &options, &output),
                  DDBERR_INVALIDARGS);

        ASSERT_EQ(DDBGetRasterValueInfoEx(path.c_str(), nullptr, &output), DDBERR_NONE);
        EXPECT_EQ(json::parse(output)["validPixelCount"], 10000);
        DDBFree(output);

        ASSERT_EQ(DDBGetRasterAreaStatsEx(path.c_str(), 14.9999, 45.1531, 15.0006, 45.1535, nullptr, &output), DDBERR_NONE);
        EXPECT_GT(json::parse(output)["pixelCount"].get<int>(), 0);
        DDBFree(output);

        const json line = {{"type", "LineString"}, {"coordinates", {{15.0001, 45.1533}, {15.0010, 45.1527}}}};
        ASSERT_EQ(DDBGetRasterProfileEx(path.c_str(), line.dump().c_str(), 8, nullptr, &output), DDBERR_NONE);
        EXPECT_GT(json::parse(output)["validCount"].get<int>(), 0);
        DDBFree(output);

        ASSERT_EQ(DDBGenerateContoursEx(path.c_str(), 5.0, 0, 0.0, std::nan(""), std::nan(""), 0.0, 1, nullptr, &output),
                  DDBERR_NONE);
        EXPECT_GT(json::parse(output)["featureCount"].get<int>(), 0);
        DDBFree(output);
    }

    // ---------------------------------------------------------------- the pixel exports with options (R-JPEG, FR-RAS-15)

    std::string callText(const std::function<DDBErr(char **)> &call, DDBErr expected = DDBERR_NONE)
    {
        char *out = nullptr;
        const DDBErr err = call(&out);
        EXPECT_EQ(err, expected) << DDBGetLastError();
        std::string text = out != nullptr ? std::string(out) : std::string();
        if (out != nullptr)
            DDBFree(out);
        return text;
    }

    DDBRasterOptions rasterOptions(const char *formula, std::optional<double> nodata = std::nullopt)
    {
        DDBRasterOptions options = {};
        options.structSize = sizeof(options);
        options.formula = formula;
        if (nodata)
        {
            options.flags = DDB_RASTER_OPT_NODATA;
            options.nodata = *nodata;
        }
        return options;
    }

    // UInt16, scale 0.1, unit Celsius: the pixel (76, 50) holds raw 291, 29.1 C. KELVIN gives 302.25 K, CELSIUS
    // 29.1 C; 291 as NoData leaves the pixel without a value. NULL options are the 1.x export plus "unit"
    TEST(RasterValuesCApi, thePixelPointTakesTheOptions)
    {
        TestArea ta(TEST_NAME, true);
        const std::string path = makeValueRaster(ta.getPath("utm.tif"), scaledSpec()).string();

        const json plain = json::parse(
            callText([&](char **out) { return DDBGetRasterPointValuePixelEx(path.c_str(), 76, 50, nullptr, out); }));
        json old = json::parse(callText([&](char **out) { return DDBGetRasterPointValue(path.c_str(), 76, 50, out); }));
        EXPECT_EQ(plain["unit"], "Celsius");
        EXPECT_EQ(plain["value"].dump(), "29.1");
        old["unit"] = "Celsius";
        EXPECT_EQ(plain, old);

        DDBRasterOptions kelvin = rasterOptions("KELVIN");
        const json k = json::parse(
            callText([&](char **out) { return DDBGetRasterPointValuePixelEx(path.c_str(), 76, 50, &kelvin, out); }));
        EXPECT_EQ(k["value"].dump(), "302.25");
        EXPECT_EQ(k["unit"], "K");
        EXPECT_EQ(k["rawValue"].get<double>(), 291.0);

        DDBRasterOptions celsius = rasterOptions("CELSIUS");
        const json c = json::parse(
            callText([&](char **out) { return DDBGetRasterPointValuePixelEx(path.c_str(), 76, 50, &celsius, out); }));
        EXPECT_EQ(c["value"].dump(), "29.1");
        EXPECT_EQ(c["unit"], "\xC2\xB0" "C");

        DDBRasterOptions nodata = rasterOptions(nullptr, 291.0);
        callText([&](char **out) { return DDBGetRasterPointValuePixelEx(path.c_str(), 76, 50, &nodata, out); },
                 DDBERR_INVALIDARGS);
        EXPECT_NE(std::string(DDBGetLastError()).find("has no value"), std::string::npos) << DDBGetLastError();

        // A formula that is not a thermal one, or none at all, gives no value at a point
        DDBRasterOptions ndvi = rasterOptions("NDVI");
        callText([&](char **out) { return DDBGetRasterPointValuePixelEx(path.c_str(), 76, 50, &ndvi, out); },
                 DDBERR_INVALIDARGS);
        DDBRasterOptions unknown = rasterOptions("NOPE");
        callText([&](char **out) { return DDBGetRasterPointValuePixelEx(path.c_str(), 76, 50, &unknown, out); },
                 DDBERR_INVALIDARGS);
        EXPECT_STREQ(DDBGetLastError(), "Unknown formula: NOPE");
        callText([&](char **out) { return DDBGetRasterPointValuePixelEx(path.c_str(), 100, 50, nullptr, out); },
                 DDBERR_INVALIDARGS);
    }

    // A raster in K: CELSIUS converts it (300 K is 26.85 C), KELVIN keeps it (D-022)
    TEST(RasterValuesCApi, theThermalFormulaConvertsFromTheUnitOfTheBand)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec;
        spec.type = GDT_Float32;
        spec.unit = "K";
        spec.value = [](int, int) { return 300.0; };
        const std::string path = makeValueRaster(ta.getPath("kelvin.tif"), spec).string();

        DDBRasterOptions celsius = rasterOptions("CELSIUS");
        const json c = json::parse(
            callText([&](char **out) { return DDBGetRasterPointValuePixelEx(path.c_str(), 5, 5, &celsius, out); }));
        EXPECT_EQ(c["value"].dump(), "26.85");
        DDBRasterOptions kelvin = rasterOptions("KELVIN");
        const json k = json::parse(
            callText([&](char **out) { return DDBGetRasterPointValuePixelEx(path.c_str(), 5, 5, &kelvin, out); }));
        EXPECT_EQ(k["value"].get<double>(), 300.0);
        EXPECT_EQ(k["unit"], "K");
    }

    // The pixels (10, 20)-(19, 29) of the scaled raster: raw 165..192, mean 17.85 C (291.0 K); with 0 as NoData the
    // 0 half of a raster is left out
    TEST(RasterValuesCApi, thePixelAreaTakesTheOptions)
    {
        TestArea ta(TEST_NAME, true);
        const std::string path = makeValueRaster(ta.getPath("utm.tif"), scaledSpec()).string();

        const json plain = json::parse(callText(
            [&](char **out) { return DDBGetRasterAreaStatsPixelEx(path.c_str(), 10, 20, 19, 29, nullptr, out); }));
        const json old =
            json::parse(callText([&](char **out) { return DDBGetRasterAreaStats(path.c_str(), 10, 20, 19, 29, out); }));
        EXPECT_EQ(plain, old);
        EXPECT_NEAR(plain["mean"].get<double>(), 17.85, 1e-5);

        DDBRasterOptions kelvin = rasterOptions("KELVIN");
        const json k = json::parse(callText(
            [&](char **out) { return DDBGetRasterAreaStatsPixelEx(path.c_str(), 10, 20, 19, 29, &kelvin, out); }));
        EXPECT_NEAR(k["mean"].get<double>(), 291.0, 1e-4);
        EXPECT_NEAR(k["min"].get<double>(), 289.65, 1e-4);
        EXPECT_EQ(k["unit"], "K");

        ValueRasterSpec half;
        half.value = [](int x, int) { return x < 50 ? 0.0 : 100.0; };
        const std::string zero = makeValueRaster(ta.getPath("zero.tif"), half).string();
        DDBRasterOptions nodata = rasterOptions(nullptr, 0.0);
        const json valid = json::parse(callText(
            [&](char **out) { return DDBGetRasterAreaStatsPixelEx(zero.c_str(), 0, 0, 99, 99, &nodata, out); }));
        EXPECT_EQ(valid["pixelCount"], 5000);
        EXPECT_EQ(valid["min"].get<double>(), 100.0);
        callText([&](char **out) { return DDBGetRasterAreaStatsPixelEx(zero.c_str(), 0, 0, 49, 99, &nodata, out); },
                 DDBERR_INVALIDARGS);
    }

    // The temperature grid of an R-JPEG GDAL does not read (3 x 2, degrees Celsius, raw counts 0 where the camera saw
    // nothing): the NoData value is compared with the raw counts, the formula converts the temperatures
    TEST(RasterValues, theTemperatureGridOfAnRJpegTakesTheOptions)
    {
        TemperatureData td;
        td.width = 3;
        td.height = 2;
        td.temperatures = {20.0f, 21.5f, -10.0f, 30.0f, 25.0f, 22.0f};
        RawThermalData raw;
        raw.width = 3;
        raw.height = 2;
        raw.valid = true;
        raw.data = {1000, 1100, 0, 1500, 1300, 1200};

        const json plain = json::parse(thermalGridPointJson(td, raw, 1, 0));
        EXPECT_EQ(plain["value"].get<double>(), 21.5);
        EXPECT_EQ(plain["rawValue"], 1100);
        EXPECT_EQ(plain["unit"], "\xC2\xB0" "C");
        EXPECT_EQ(plain["hasGeo"], false);

        RasterValueOptions kelvin;
        kelvin.formula = "KELVIN";
        const json k = json::parse(thermalGridPointJson(td, raw, 1, 0, kelvin));
        EXPECT_EQ(k["value"].dump(), "294.65");
        EXPECT_EQ(k["unit"], "K");

        RasterValueOptions nodata;
        nodata.nodata = 0.0;
        EXPECT_THROW(thermalGridPointJson(td, raw, 2, 0, nodata), NoRasterDataException);
        EXPECT_EQ(json::parse(thermalGridPointJson(td, raw, 2, 0))["value"].get<double>(), -10.0);
        EXPECT_THROW(thermalGridPointJson(td, raw, 3, 0), NoRasterDataException);

        // The area: -10 is in it without the NoData value, out of it with
        const json all = json::parse(thermalGridAreaStatsJson(td, raw, 0, 0, 2, 1));
        EXPECT_EQ(all["pixelCount"], 6);
        EXPECT_EQ(all["min"].get<double>(), -10.0);
        RasterValueOptions both;
        both.nodata = 0.0;
        both.formula = "KELVIN";
        const json valid = json::parse(thermalGridAreaStatsJson(td, raw, 0, 0, 2, 1, both));
        EXPECT_EQ(valid["pixelCount"], 5);
        EXPECT_EQ(valid["min"].dump(), "293.15");
        EXPECT_EQ(valid["max"].dump(), "303.15");
        EXPECT_EQ(valid["unit"], "K");

        RasterValueOptions ndvi;
        ndvi.formula = "NDVI";
        EXPECT_THROW(thermalGridAreaStatsJson(td, raw, 0, 0, 2, 1, ndvi), RefusedArgumentException);
    }

    TEST(RasterValuesCApi, invalidOptionsAreRefused)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = makeValueRaster(ta.getPath("utm.tif"), scaledSpec());
        const std::string path = raster.string();
        char *output = nullptr;

        DDBRasterOptions options = {};
        options.structSize = sizeof(options);
        options.flags = DDB_RASTER_OPT_NODATA;
        options.nodata = std::nan("");
        EXPECT_EQ(DDBGetRasterValueInfoEx(path.c_str(), &options, &output), DDBERR_INVALIDARGS);

        options.flags = 0x80;
        EXPECT_EQ(DDBGetRasterValueInfoEx(path.c_str(), &options, &output), DDBERR_INVALIDARGS);

        options.flags = 0;
        options.structSize = 4;
        EXPECT_EQ(DDBGetRasterValueInfoEx(path.c_str(), &options, &output), DDBERR_INVALIDARGS);

        EXPECT_EQ(DDBGetRasterPointValueEx(path.c_str(), std::nan(""), 45.0, nullptr, &output), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBGetRasterAreaStatsEx(path.c_str(), 1, 2, std::nan(""), 4, nullptr, &output), DDBERR_INVALIDARGS);
    }

    // The NoData override reaches the thumbnail (alpha 0 over the 0 background), the tile and the export
    TEST(RasterValuesCApi, theNodataOverrideReachesThumbnailAndExport)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec;
        spec.epsg = 3857;
        spec.originX = 1670000.0;
        spec.originY = 5645600.0;
        spec.type = GDT_Float32;
        spec.value = [](int x, int) { return x < 50 ? 0.0 : 10.0 + x; };
        const auto raster = makeValueRaster(ta.getPath("zero.tif"), spec);
        const std::string path = raster.string();

        DDBRasterOptions options = {};
        options.structSize = sizeof(options);
        options.flags = DDB_RASTER_OPT_NODATA;
        options.nodata = 0.0;

        uint8_t *buffer = nullptr;
        int size = 0;
        ASSERT_EQ(DDBGenerateMemoryThumbnailEx2(path.c_str(), 100, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                &options, &buffer, &size),
                  DDBERR_NONE);
        const auto thumb = testutils::decodeImage(buffer, static_cast<size_t>(size));
        DDBVSIFree(buffer);
        ASSERT_TRUE(thumb.hasAlpha());
        // The left half is no data: transparent; the right half is opaque
        EXPECT_EQ(thumb.alpha()[50 * thumb.width + 10], 0);
        EXPECT_EQ(thumb.alpha()[50 * thumb.width + 90], 255);

        const std::string out = ta.getPath("export.tif").string();
        ASSERT_EQ(DDBExportRaster3(path.c_str(), out.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0,
                                   nullptr, nullptr, &options),
                  DDBERR_NONE);
        GDALDatasetH hExport = GDALOpen(out.c_str(), GA_ReadOnly);
        ASSERT_NE(hExport, nullptr);
        EXPECT_GE(GDALGetRasterCount(hExport), 1);
        GDALClose(hExport);
    }

    // ---------------------------------------------------------------- contours (FR-RAS-04, FR-RAS-31)

    // Raw 115..214 with a scale of 0.1 is 11.5..21.4: the levels of a 2 interval are 12, 14, ... 20
    TEST(RasterValues, theContourLevelsAreInPhysicalValues)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec = scaledSpec();
        spec.epsg = 4326;
        spec.originX = 10.0;
        spec.originY = 45.0;
        spec.pixel = 0.0001;
        spec.value = [](int x, int) { return 115.0 + x; };
        const auto raster = makeValueRaster(ta.getPath("scaled-dem.tif"), spec);

        ContourOptions options;
        options.interval = 2.0;
        const json contours = json::parse(generateContoursJson(raster.string(), options));
        EXPECT_NEAR(contours["rasterMin"].get<double>(), 11.5, 1e-9);
        EXPECT_NEAR(contours["rasterMax"].get<double>(), 21.4, 1e-9);
        EXPECT_EQ(contours["min"].get<double>(), 12.0);
        EXPECT_EQ(contours["max"].get<double>(), 20.0);
        EXPECT_EQ(contours["featureCount"], 5);
        EXPECT_EQ(contours["unit"], "Celsius");
    }

    TEST(RasterValues, theContoursLeaveOutTheOverriddenNodataValue)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec;
        spec.epsg = 4326;
        spec.originX = 10.0;
        spec.originY = 45.0;
        spec.pixel = 0.0001;
        spec.value = [](int x, int) { return x < 20 ? 0.0 : 100.0 + x; };
        const auto raster = makeValueRaster(ta.getPath("background.tif"), spec);

        ContourOptions options;
        options.count = 4;
        options.nodata = 0.0;
        const json contours = json::parse(generateContoursJson(raster.string(), options));
        // The range of the statistics is that of the pixels that have a value: 120..199
        EXPECT_EQ(contours["rasterMin"].get<double>(), 120.0);
        EXPECT_EQ(contours["rasterMax"].get<double>(), 199.0);
        EXPECT_GE(contours["min"].get<double>(), 120.0);
    }

    // A raster in EPSG:4326 whose 20 left columns are `background` and the others 100 + x (120..199), and a mask of
    // its own that hides the 20 left columns: an alpha band (GDAL takes a Byte or UInt16 band as alpha, so the
    // raster is UInt16, the alpha 0 or 65535), a dataset mask (Float32), or none (Float32)
    enum class ContourMask
    {
        None,
        Alpha,
        DatasetMask
    };

    fs::path makeContourRaster(const fs::path &path, double background, ContourMask mask)
    {
        GDALDriverH drv = GDALGetDriverByName("GTiff");
        const int bands = mask == ContourMask::Alpha ? 2 : 1;
        const GDALDataType type = mask == ContourMask::Alpha ? GDT_UInt16 : GDT_Float32;
        constexpr int size = 100;
        GDALDatasetH hDs = GDALCreate(drv, path.string().c_str(), size, size, bands, type, nullptr);
        if (!hDs)
            throw std::runtime_error("Cannot create the test raster");
        const double gt[6] = {10.0, 0.0001, 0.0, 45.0, 0.0, -0.0001};
        GDALSetGeoTransform(hDs, const_cast<double *>(gt));
        GDALSetProjection(hDs, SRS_WKT_WGS84_LAT_LONG);

        std::vector<float> values(static_cast<size_t>(size) * size);
        std::vector<uint8_t> opaque(values.size());
        std::vector<uint16_t> alphaValues(values.size());
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
            {
                const size_t i = static_cast<size_t>(y) * size + x;
                values[i] = x < 20 ? static_cast<float>(background) : static_cast<float>(100 + x);
                opaque[i] = x < 20 ? 0 : 255;
                alphaValues[i] = x < 20 ? 0 : 65535;
            }
        EXPECT_EQ(GDALRasterIO(GDALGetRasterBand(hDs, 1), GF_Write, 0, 0, size, size, values.data(), size, size,
                               GDT_Float32, 0, 0),
                  CE_None);
        if (mask == ContourMask::Alpha)
        {
            GDALRasterBandH alpha = GDALGetRasterBand(hDs, 2);
            GDALSetRasterColorInterpretation(alpha, GCI_AlphaBand);
            EXPECT_EQ(GDALRasterIO(alpha, GF_Write, 0, 0, size, size, alphaValues.data(), size, size, GDT_UInt16, 0, 0),
                      CE_None);
        }
        else if (mask == ContourMask::DatasetMask)
        {
            GDALCreateDatasetMaskBand(hDs, GMF_PER_DATASET);
            EXPECT_EQ(GDALRasterIO(GDALGetMaskBand(GDALGetRasterBand(hDs, 1)), GF_Write, 0, 0, size, size,
                                   opaque.data(), size, size, GDT_Byte, 0, 0),
                      CE_None);
        }
        GDALFlushCache(hDs);
        GDALClose(hDs);
        return path;
    }

    // FR-RAS-31, FR-XC-24: a pixel the alpha band or the dataset mask hides has no value, so no contour crosses the
    // transparent area of a mosaic. With a 10 interval the lines of the data (120..199) are the levels 130..190 (120 is
    // its edge column, next to the pixels without a value); the 0 behind the mask drew the levels 10..110 along it
    TEST(RasterValues, theContoursLeaveOutTheMaskedPixels)
    {
        TestArea ta(TEST_NAME, true);
        for (const ContourMask mask : {ContourMask::Alpha, ContourMask::DatasetMask})
        {
            const std::string name = mask == ContourMask::Alpha ? "alpha" : "mask";
            const auto raster = makeContourRaster(ta.getPath(name + ".tif"), 0.0, mask);

            ContourOptions options;
            options.interval = 10.0;
            const json contours = json::parse(generateContoursJson(raster.string(), options));
            EXPECT_EQ(contours["rasterMin"].get<double>(), 120.0) << name;
            EXPECT_EQ(contours["rasterMax"].get<double>(), 199.0) << name;
            ASSERT_FALSE(contours["min"].is_null()) << name;
            EXPECT_EQ(contours["min"].get<double>(), 130.0) << name;
            EXPECT_EQ(contours["max"].get<double>(), 190.0) << name;
            for (const auto &feature : contours["features"])
                EXPECT_GE(feature["properties"]["elev"].get<double>(), 120.0) << name;
        }

        // The same raster without a mask draws the edge of the 0 area: the band-value model is what leaves it out
        const auto plain = makeContourRaster(ta.getPath("plain.tif"), 0.0, ContourMask::None);
        ContourOptions options;
        options.interval = 10.0;
        EXPECT_EQ(json::parse(generateContoursJson(plain.string(), options))["min"].get<double>(), 10.0);
    }

    // The NoData value reaches the generator with all its digits: 1e-7 was "0.000000" (std::to_string), so the 1e-7
    // background was data and drew the levels 10..110 along its edge
    TEST(RasterValues, aTinyNodataValueKeepsItsDigitsInTheContours)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = makeContourRaster(ta.getPath("tiny.tif"), 1e-7, ContourMask::None);

        ContourOptions options;
        options.interval = 10.0;
        options.nodata = 1e-7;
        const json contours = json::parse(generateContoursJson(raster.string(), options));
        // The statistics compare the Float32 samples with the NoData value rounded to float, as GDAL does
        EXPECT_EQ(contours["rasterMin"].get<double>(), 120.0);
        ASSERT_FALSE(contours["min"].is_null());
        EXPECT_EQ(contours["min"].get<double>(), 130.0);
        EXPECT_EQ(contours["max"].get<double>(), 190.0);

        // The base of the levels keeps its digits too: 4.9e-7 was "0.000000", which put the levels on 130, 140, ...
        ContourOptions shifted = options;
        shifted.baseOffset = 4.9e-7;
        const json levels = json::parse(generateContoursJson(raster.string(), shifted));
        ASSERT_FALSE(levels["min"].is_null());
        EXPECT_NEAR(levels["min"].get<double>(), 130.00000049, 1e-9);
    }

    // ---------------------------------------------------------------- tiles (FR-RAS-31, FR-RAS-21)

    struct TileAt
    {
        int z, x, y;
    };

    testutils::DecodedImage tileOf(const std::string &path, const TileAt &tile, const DDBRasterOptions *options)
    {
        uint8_t *buffer = nullptr;
        int size = 0;
        const DDBErr err = DDBMemoryTileEx2(path.c_str(), tile.z, tile.x, tile.y, 256, false, true, nullptr, nullptr,
                                            nullptr, nullptr, nullptr, nullptr, nullptr, options, &buffer, &size);
        EXPECT_EQ(err, DDBERR_NONE);
        const auto image = testutils::decodeImage(buffer, static_cast<size_t>(size));
        DDBVSIFree(buffer);
        return image;
    }

    size_t countOpaque(const testutils::DecodedImage &image)
    {
        if (!image.hasAlpha())
            return static_cast<size_t>(image.width) * image.height;
        return static_cast<size_t>(std::count_if(image.alpha().begin(), image.alpha().end(), [](uint8_t a) { return a > 0; }));
    }

    // The tile of the deepest zoom that the raster covers most
    TileAt fullestTile(const fs::path &raster)
    {
        GDALTiler t(TilerHelper::toGeoTIFF(raster).string(), "", 256);
        const int z = t.getMinMaxZ().max;
        const auto b = t.getMinMaxCoordsForZ(z);
        TileAt best{z, b.min.x, b.min.y};
        size_t bestCount = 0;
        for (int x = b.min.x; x <= b.max.x; x++)
            for (int y = b.min.y; y <= b.max.y; y++)
            {
                const size_t count = countOpaque(tileOf(raster.string(), {z, x, y}, nullptr));
                if (count > bestCount)
                {
                    bestCount = count;
                    best = {z, x, y};
                }
            }
        return best;
    }

    // 16-bit data with a zero background and no NoData value: the background is data until the override says it
    // is not, and the tile is transparent there
    TEST(RasterValues, theTilesAreTransparentOverTheOverriddenNodataValue)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec;
        spec.epsg = 3857;
        spec.originX = 1670000.0;
        spec.originY = 5645600.0;
        spec.pixel = 4.0; // 400 m: the tile at the deepest zoom covers it in two or three tiles
        spec.value = [](int x, int) { return x < 50 ? 0.0 : 1000.0 + x * 10.0; };
        const auto raster = makeValueRaster(ta.getPath("hat.tif"), spec);
        const TileAt tile = fullestTile(raster);
        const std::string path = raster.string();

        const auto plain = tileOf(path, tile, nullptr);
        DDBRasterOptions options = {};
        options.structSize = sizeof(options);
        options.flags = DDB_RASTER_OPT_NODATA;
        options.nodata = 0.0;
        const auto overridden = tileOf(path, tile, &options);

        EXPECT_LT(countOpaque(overridden), countOpaque(plain));
        EXPECT_GT(countOpaque(overridden), 0u);
        // The stretch is that of the pixels that have a value: the brightest data pixel is white
        EXPECT_EQ(*std::max_element(overridden.bands[0].begin(), overridden.bands[0].end()), 255);
    }

    // A thermal mosaic (Float32, one band, 18..45 C) with a 0 background and no NoData value, shown with 0 as NoData:
    // the default tile is stretched over 18..45, the stretch value info returns, not over 0..45 (which put 18 C at
    // grey 102 of 255)
    TEST(RasterValues, theDefaultTileOfAThermalMosaicIsStretchedLikeTheValueInfoWithTheOverride)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec;
        spec.epsg = 3857;
        spec.originX = 1670000.0;
        spec.originY = 5645600.0;
        spec.pixel = 4.0;
        spec.type = GDT_Float32;
        // Every 28 pixels along a diagonal the values run 18..45, so every tile holds the coldest and the hottest
        spec.value = [](int x, int y) { return x < 10 ? 0.0 : 18.0 + ((x + y) % 28); };
        const auto raster = makeValueRaster(ta.getPath("thermal.tif"), spec);
        const std::string path = raster.string();

        RasterValueOptions valueOptions;
        valueOptions.nodata = 0.0;
        const json info = json::parse(getRasterValueInfoJson(path, valueOptions));
        ASSERT_EQ(info["stretch"].size(), 1u);
        EXPECT_EQ(info["stretch"][0]["min"].get<double>(), 18.0);
        EXPECT_EQ(info["stretch"][0]["max"].get<double>(), 45.0);

        const TileAt tile = fullestTile(raster);
        DDBRasterOptions options = {};
        options.structSize = sizeof(options);
        options.flags = DDB_RASTER_OPT_NODATA;
        options.nodata = 0.0;
        const auto image = tileOf(path, tile, &options);
        uint8_t darkest = 255, brightest = 0;
        size_t opaque = 0;
        for (size_t i = 0; i < image.bands[0].size(); i++)
        {
            if (image.hasAlpha() && image.alpha()[i] == 0)
                continue;
            darkest = std::min(darkest, image.bands[0][i]);
            brightest = std::max(brightest, image.bands[0][i]);
            opaque++;
        }
        ASSERT_GT(opaque, 1000u);
        // 18 C is black and 45 C white, as with the stretch of value info (18..45)
        EXPECT_LE(darkest, 5);
        EXPECT_GE(brightest, 250);
    }

    // FR-RAS-21: 16-bit bands whose range holds a few very bright pixels (specular points) open readable, not black
    TEST(RasterValues, aSixteenBitMultiBandTileIsNotAlmostBlack)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = 256;
        spec.height = 256;
        spec.bands = 3;
        spec.type = GDT_UInt16;
        spec.pixelSize = 2.0;
        spec.value = [](int band, int x, int y) {
            if ((x == 10 && y == 10) || (x == 200 && y == 30))
                return 60000.0; // the bright outliers that stretch the range of every band
            return 500.0 * band + 4.0 * ((x * 3 + y * 7) % 400);
        };
        const auto raster = testutils::createGeoRaster(ta.getPath("ortho16.tif"), spec);
        const TileAt tile = fullestTile(raster);

        const auto image = tileOf(raster.string(), tile, nullptr);
        double sum = 0;
        size_t count = 0;
        for (size_t i = 0; i < image.bands[0].size(); i++)
        {
            if (image.hasAlpha() && image.alpha()[i] == 0)
                continue;
            sum += image.bands[0][i];
            count++;
        }
        ASSERT_GT(count, 1000u);
        // One range over 0..41000 for all the bands put the mean at about 4 of 255 (607 of 41127); the range of
        // the band, mean + 2.33 standard deviations (about 2300), puts it at about 40
        EXPECT_GT(sum / static_cast<double>(count), 20.0);
    }

    // ---------------------------------------------------------------- the COG build keeps the band model (D-021)

    TEST(RasterValues, theCogKeepsTheScaleTheOffsetTheUnitAndTheNodata)
    {
        TestArea ta(TEST_NAME, true);
        ValueRasterSpec spec = scaledSpec();
        spec.offset = 1.5;
        spec.nodata = 0.0;
        spec.value = [](int x, int y) { return x < 10 ? 0.0 : 115.0 + x + 2.0 * y; };
        const auto raster = makeValueRaster(ta.getPath("scaled.tif"), spec);
        const fs::path cog = ta.getPath("scaled-cog.tif");

        buildCog(raster.string(), cog.string(), BuildOptions(), true);

        GDALDatasetH hDs = GDALOpen(cog.string().c_str(), GA_ReadOnly);
        ASSERT_NE(hDs, nullptr);
        GDALRasterBandH band = GDALGetRasterBand(hDs, 1);
        int hasScale = 0, hasOffset = 0, hasNodata = 0;
        EXPECT_DOUBLE_EQ(GDALGetRasterScale(band, &hasScale), 0.1);
        EXPECT_DOUBLE_EQ(GDALGetRasterOffset(band, &hasOffset), 1.5);
        EXPECT_TRUE(hasScale);
        EXPECT_TRUE(hasOffset);
        EXPECT_STREQ(GDALGetRasterUnitType(band), "Celsius");
        EXPECT_DOUBLE_EQ(GDALGetRasterNoDataValue(band, &hasNodata), 0.0);
        EXPECT_TRUE(hasNodata);
        GDALClose(hDs);
    }

    // The same without a NoData value, and for a raster that is on the tile grid already (it is encoded again,
    // not warped)
    TEST(RasterValues, theCogOfARasterWithoutNodataKeepsTheScaleToo)
    {
        TestArea ta(TEST_NAME, true);
        for (const int epsg : {32633, 3857})
        {
            ValueRasterSpec spec = scaledSpec();
            spec.epsg = epsg;
            if (epsg == 3857)
            {
                spec.originX = 1670000.0;
                spec.originY = 5645600.0;
            }
            const auto raster = makeValueRaster(ta.getPath("scaled-" + std::to_string(epsg) + ".tif"), spec);
            const fs::path cog = ta.getPath("cog-" + std::to_string(epsg) + ".tif");
            buildCog(raster.string(), cog.string(), BuildOptions(), true);

            GDALDatasetH hDs = GDALOpen(cog.string().c_str(), GA_ReadOnly);
            ASSERT_NE(hDs, nullptr) << epsg;
            GDALRasterBandH band = GDALGetRasterBand(hDs, 1);
            int hasScale = 0;
            EXPECT_DOUBLE_EQ(GDALGetRasterScale(band, &hasScale), 0.1) << epsg;
            EXPECT_TRUE(hasScale) << epsg;
            EXPECT_STREQ(GDALGetRasterUnitType(band), "Celsius") << epsg;
            GDALClose(hDs);
        }
    }
}
