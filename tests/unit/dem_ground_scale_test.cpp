/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The areas, volumes and lengths of the elevation analyses are ground meters, whatever the CRS of
// the DEM: its pixels are in the linear unit of the CRS (meters, feet, US survey feet) and a
// projection stretches the ground (Web Mercator by 1 / cos(latitude)). One terrain, a flat plane
// with a block of known size on it, is written in each CRS and measured through the public
// analyses; the elevations of a DEM in feet are converted to meters the same way.

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "dem.h"
#include "ddb.h"
#include "gdal_inc.h"
#include "json.h"
#include "mathutil.h"
#include "raster_profile.h"
#include "stockpile.h"
#include "volume.h"

namespace
{
    using namespace ddb;

    constexpr double kSiteLon = -121.5; // UTM zone 10, 1.5 degrees from its central meridian
    constexpr double kSiteLat = 38.0;
    constexpr double kBlockHalf = 30.0;   // meters: the block is 60 x 60 m
    constexpr double kBlockHeight = 3.0;  // meters above the plane
    constexpr double kPlane = 100.0;      // meters
    constexpr int kUtm10 = 32610;
    constexpr int kWebMercator = 3857;
    constexpr int kCaliforniaZone3Feet = 2227; // NAD83 / California zone 3 (ftUS)
    constexpr double kFoot = 0.3048;
    constexpr double kUsSurveyFoot = 1200.0 / 3937.0;

    OGRSpatialReference srsOfEpsg(int epsg)
    {
        OGRSpatialReference srs;
        srs.importFromEPSG(epsg);
        srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        return srs;
    }

    // Moves (x, y) from a CRS to another, the axes always being x = east
    std::pair<double, double> convert(int fromEpsg, int toEpsg, double x, double y)
    {
        const OGRSpatialReference from = srsOfEpsg(fromEpsg), to = srsOfEpsg(toEpsg);
        std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&from, &to));
        if (!ct || !ct->Transform(1, &x, &y))
            throw std::runtime_error("Cannot transform a test point");
        return {x, y};
    }

    // A site in meters east and north of the site, in UTM, as longitude and latitude
    std::pair<double, double> siteOffset(double east, double north)
    {
        const auto [e0, n0] = convert(4326, kUtm10, kSiteLon, kSiteLat);
        return convert(kUtm10, 4326, e0 + east, n0 + north);
    }

    struct DemOptions
    {
        int epsg = kUtm10;
        double pixel = 1.0;          // in the unit of the CRS: one meter on the ground
        int size = 240;
        std::string zUnit;           // the unit type of the band, "" when it has none
        double zPerMeter = 1.0;      // the elevations are written in that many units per meter
    };

    // The terrain, in the CRS asked: the plane with a block of 60 x 60 m at the site. The block is
    // placed on the ground (UTM axes) and not in the pixels, so it is the same in every CRS.
    fs::path makeDem(const fs::path &path, const DemOptions &opts)
    {
        GDALDriver *drv = GetGDALDriverManager()->GetDriverByName("GTiff");
        std::unique_ptr<GDALDataset> ds(drv->Create(path.string().c_str(), opts.size, opts.size, 1, GDT_Float32, nullptr));
        if (!ds)
            throw std::runtime_error("Cannot create " + path.string());

        const auto [cx, cy] = convert(4326, opts.epsg, kSiteLon, kSiteLat);
        const double originX = cx - opts.size * opts.pixel / 2.0;
        const double originY = cy + opts.size * opts.pixel / 2.0;
        double gt[6] = {originX, opts.pixel, 0.0, originY, 0.0, -opts.pixel};
        ds->SetGeoTransform(gt);
        char *wkt = nullptr;
        srsOfEpsg(opts.epsg).exportToWkt(&wkt);
        ds->SetProjection(wkt);
        CPLFree(wkt);

        const OGRSpatialReference from = srsOfEpsg(opts.epsg), utm = srsOfEpsg(kUtm10);
        std::unique_ptr<OGRCoordinateTransformation> toUtm(OGRCreateCoordinateTransformation(&from, &utm));
        const auto [e0, n0] = convert(4326, kUtm10, kSiteLon, kSiteLat);

        GDALRasterBand *band = ds->GetRasterBand(1);
        std::vector<float> row(opts.size);
        for (int y = 0; y < opts.size; y++)
        {
            for (int x = 0; x < opts.size; x++)
            {
                double e = originX + (x + 0.5) * opts.pixel, n = originY - (y + 0.5) * opts.pixel;
                if (!toUtm || !toUtm->Transform(1, &e, &n))
                    throw std::runtime_error("Cannot transform a pixel");
                const bool inBlock = std::fabs(e - e0) < kBlockHalf && std::fabs(n - n0) < kBlockHalf;
                row[x] = static_cast<float>((kPlane + (inBlock ? kBlockHeight : 0.0)) * opts.zPerMeter);
            }
            if (band->RasterIO(GF_Write, 0, y, opts.size, 1, row.data(), opts.size, 1, GDT_Float32, 0, 0) != CE_None)
                throw std::runtime_error("Cannot write the test DEM");
        }
        if (!opts.zUnit.empty())
            band->SetUnitType(opts.zUnit.c_str());
        band->SetNoDataValue(-9999.0);
        ds->FlushCache();
        return path;
    }

    // The ground square of `half` meters around the site, in WGS84
    std::string squareAroundSite(double half)
    {
        json ring = json::array();
        for (const auto &[e, n] : std::vector<std::pair<double, double>>{{-half, -half}, {half, -half}, {half, half}, {-half, half}, {-half, -half}})
        {
            const auto [lon, lat] = siteOffset(e, n);
            ring.push_back({lon, lat});
        }
        return json{{"type", "Polygon"}, {"coordinates", json::array({ring})}}.dump();
    }

    // The CRSs of the same terrain, with the size of a one meter pixel in their unit
    struct CrsCase
    {
        const char *name;
        DemOptions opts;
    };

    std::vector<CrsCase> crsCases()
    {
        DemOptions utm;
        DemOptions web;
        web.epsg = kWebMercator;
        web.pixel = 1.0 / std::cos(kSiteLat * kPi / 180.0);
        DemOptions feet;
        feet.epsg = kCaliforniaZone3Feet;
        feet.pixel = 1.0 / kUsSurveyFoot;
        return {{"utm", utm}, {"web", web}, {"usSurveyFeet", feet}};
    }

    class DemGroundScale : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    TEST_F(DemGroundScale, volumeIsTheSameTerrainInEveryCrs)
    {
        TestArea ta(TEST_NAME, true);
        const std::string polygon = squareAroundSite(50.0);
        for (const auto &c : crsCases())
        {
            SCOPED_TRACE(c.name);
            const fs::path dem = makeDem(ta.getPath(std::string(c.name) + ".tif"), c.opts);
            const json r = json::parse(calculateVolumeJson(dem.string(), polygon, "flat", kPlane));
            // The polygon is 100 x 100 m and holds the whole block of 60 x 60 x 3 m
            EXPECT_NEAR(r["area2d"].get<double>(), 10000.0, 100.0);
            EXPECT_NEAR(r["cutVolume"].get<double>(), 3600.0 * kBlockHeight, 150.0);
            EXPECT_NEAR(r["pixelSize"].get<double>(), 1.0, 0.01);
        }
    }

    TEST_F(DemGroundScale, area3dIsTheSameInEveryCrs)
    {
        // The 3D area follows the slope, which divides the rise by the ground size of a pixel
        TestArea ta(TEST_NAME, true);
        const std::string polygon = squareAroundSite(50.0);
        double area3d[3] = {};
        int i = 0;
        for (const auto &c : crsCases())
        {
            const fs::path dem = makeDem(ta.getPath(std::string(c.name) + ".tif"), c.opts);
            const json r = json::parse(calculateVolumeJson(dem.string(), polygon, "flat", kPlane));
            area3d[i++] = r["area3d"].get<double>();
        }
        EXPECT_NEAR(area3d[1], area3d[0], area3d[0] * 0.02);
        EXPECT_NEAR(area3d[2], area3d[0], area3d[0] * 0.02);
    }

    TEST_F(DemGroundScale, pixelSizeIsMeasuredOnTheGround)
    {
        TestArea ta(TEST_NAME, true);
        for (const auto &c : crsCases())
        {
            SCOPED_TRACE(c.name);
            const fs::path path = makeDem(ta.getPath(std::string(c.name) + ".tif"), c.opts);
            const DemContext dem = openDem(path.string());
            const auto [x, y] = convert(4326, c.opts.epsg, kSiteLon, kSiteLat);
            const PixelSize size = dem.pixelSizeMeters(x, y);
            EXPECT_NEAR(size.x, 1.0, 0.005);
            EXPECT_NEAR(size.y, 1.0, 0.005);
        }
    }

    TEST_F(DemGroundScale, elevationsInFeetAreConvertedToMeters)
    {
        TestArea ta(TEST_NAME, true);
        const std::string polygon = squareAroundSite(50.0);
        int n = 0;
        for (const auto &[unit, metersPerUnit] : std::vector<std::pair<std::string, double>>{
                 {"ft", kFoot}, {"US survey foot", kUsSurveyFoot}, {"m", 1.0}, {"", 1.0}})
        {
            SCOPED_TRACE("unit '" + unit + "'");
            DemOptions opts;
            opts.zUnit = unit;
            opts.zPerMeter = 1.0 / metersPerUnit;
            const fs::path dem = makeDem(ta.getPath("z" + std::to_string(n++) + ".tif"), opts);
            const json r = json::parse(calculateVolumeJson(dem.string(), polygon, "lowest_perimeter", 0.0));
            EXPECT_NEAR(r["baseElevation"].get<double>(), kPlane, 0.05);
            EXPECT_NEAR(r["cutVolume"].get<double>(), 3600.0 * kBlockHeight, 150.0);
        }
    }

    TEST_F(DemGroundScale, flatElevationOfADemInFeetIsInMeters)
    {
        // The flat base plane is given in meters, like the elevations it is compared to, whatever
        // the unit of the band
        TestArea ta(TEST_NAME, true);
        const std::string polygon = squareAroundSite(50.0);
        int n = 0;
        for (const auto &[unit, metersPerUnit] : std::vector<std::pair<std::string, double>>{
                 {"ft", kFoot}, {"US survey foot", kUsSurveyFoot}})
        {
            SCOPED_TRACE("unit '" + unit + "'");
            DemOptions opts;
            opts.zUnit = unit;
            opts.zPerMeter = 1.0 / metersPerUnit;
            const fs::path dem = makeDem(ta.getPath("flat" + std::to_string(n++) + ".tif"), opts);
            const json r = json::parse(calculateVolumeJson(dem.string(), polygon, "flat", kPlane));
            EXPECT_NEAR(r["baseElevation"].get<double>(), kPlane, 1e-6);
            // Only the block rises above the plane: 60 x 60 x 3 m, nothing to fill
            EXPECT_NEAR(r["cutVolume"].get<double>(), 3600.0 * kBlockHeight, 150.0);
            EXPECT_NEAR(r["fillVolume"].get<double>(), 0.0, 1.0);
        }
    }

    TEST_F(DemGroundScale, profileOfADemInFeetIsInTheUnitOfTheBand)
    {
        TestArea ta(TEST_NAME, true);
        DemOptions opts;
        opts.zUnit = "ft";
        opts.zPerMeter = 1.0 / kFoot;
        const fs::path dem = makeDem(ta.getPath("profile-ft.tif"), opts);

        // 20 m across the block, which is 3 m above the plane
        json coords = json::array();
        for (const auto &[e, n] : std::vector<std::pair<double, double>>{{-10.0, 0.0}, {10.0, 0.0}})
        {
            const auto [lon, lat] = siteOffset(e, n);
            coords.push_back({lon, lat});
        }
        const json line = {{"type", "LineString"}, {"coordinates", coords}};
        const json r = json::parse(getRasterProfileJson(dem.string(), line.dump(), 5));
        EXPECT_EQ(r["unit"].get<std::string>(), "ft");
        EXPECT_NEAR(r["min"].get<double>(), (kPlane + kBlockHeight) / kFoot, 1e-2);
        EXPECT_NEAR(r["max"].get<double>(), (kPlane + kBlockHeight) / kFoot, 1e-2);
    }

    TEST_F(DemGroundScale, elevationUnitTypes)
    {
        EXPECT_DOUBLE_EQ(elevationUnitToMeters(""), 1.0);
        EXPECT_DOUBLE_EQ(elevationUnitToMeters("m"), 1.0);
        EXPECT_DOUBLE_EQ(elevationUnitToMeters("metre"), 1.0);
        EXPECT_DOUBLE_EQ(elevationUnitToMeters("W/m2"), 1.0);
        for (const char *foot : {"ft", "FT", "foot", "Feet", "International foot"})
            EXPECT_DOUBLE_EQ(elevationUnitToMeters(foot), kFoot) << foot;
        for (const char *foot : {"US survey foot", "ftUS", "us-ft", "US Survey Feet"})
            EXPECT_DOUBLE_EQ(elevationUnitToMeters(foot), kUsSurveyFoot) << foot;
    }

    TEST_F(DemGroundScale, nodataOfAConvertedDemStaysNodata)
    {
        TestArea ta(TEST_NAME, true);
        DemOptions opts;
        opts.size = 20;
        opts.zUnit = "ft";
        const fs::path path = makeDem(ta.getPath("nodata.tif"), opts);
        {
            std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(path.string().c_str(), GA_Update)));
            const float nodata = -9999.0f;
            ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 3, 3, 1, 1, const_cast<float *>(&nodata), 1, 1, GDT_Float32, 0, 0), CE_None);
        }
        const DemContext dem = openDem(path.string());
        const std::vector<float> samples = dem.readElevations(0, 0, 20, 20);
        EXPECT_FALSE(dem.isValid(samples[3 * 20 + 3]));
        EXPECT_TRUE(dem.isValid(samples[0]));
        // The 20 x 20 m around the site are all on the block
        EXPECT_NEAR(samples[0], (kPlane + kBlockHeight) * kFoot, 1e-3);
    }

    TEST_F(DemGroundScale, profileLengthIsTheGroundLength)
    {
        TestArea ta(TEST_NAME, true);
        for (const auto &c : crsCases())
        {
            SCOPED_TRACE(c.name);
            const fs::path dem = makeDem(ta.getPath(std::string(c.name) + ".tif"), c.opts);
            // 100 m east-west, then 50 m north
            json coords = json::array();
            for (const auto &[e, n] : std::vector<std::pair<double, double>>{{-50.0, -25.0}, {50.0, -25.0}, {50.0, 25.0}})
            {
                const auto [lon, lat] = siteOffset(e, n);
                coords.push_back({lon, lat});
            }
            const json line = {{"type", "LineString"}, {"coordinates", coords}};
            const json r = json::parse(getRasterProfileJson(dem.string(), line.dump(), 31));
            EXPECT_NEAR(r["totalLength"].get<double>(), 150.0, 1.0);
            // The samples are spread over the ground: the one at the corner of the line is 100 m in
            EXPECT_NEAR(r["samples"][20]["distance"].get<double>(), 100.0, 1.0);
        }
    }

    TEST_F(DemGroundScale, stockpileOfTheBlockIsTheSameInEveryCrs)
    {
        TestArea ta(TEST_NAME, true);
        const auto [lon, lat] = siteOffset(0.0, 0.0);
        std::vector<double> volumes;
        for (const auto &c : crsCases())
        {
            SCOPED_TRACE(c.name);
            const fs::path dem = makeDem(ta.getPath(std::string(c.name) + ".tif"), c.opts);
            const json r = json::parse(detectStockpileJson(dem.string(), lat, lon, 80.0, 0.5f));
            volumes.push_back(r["estimatedVolume"].get<double>());
            // The detection smooths the edges of the block: a bit less than its volume
            EXPECT_NEAR(volumes.back(), 3600.0 * kBlockHeight, 3600.0 * kBlockHeight * 0.15);
        }
        ASSERT_EQ(volumes.size(), 3u);
        EXPECT_NEAR(volumes[1], volumes[0], volumes[0] * 0.02);
        EXPECT_NEAR(volumes[2], volumes[0], volumes[0] * 0.02);
    }
}
