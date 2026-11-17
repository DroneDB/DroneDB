/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Point clouds whose coordinate reference system cannot be used as a whole: a compound CRS whose
// vertical part is bound to a geoid grid that is not installed (DJI Terra writes them), and a CRS
// that has no transformation to WGS84. The footprint is computed from the horizontal CRS, and when it
// cannot be, the file is still indexed and built, without a footprint and with a warning.
//
// Point clouds in any CRS: a geographic or geocentric cloud is indexed with its status and the
// projected CRS its COPC is built in, and its COPC is built in it; a CRS the coordinates do not fit
// is ignored with a warning; a geocentric footprint is transformed in 3-D.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <cmath>
#include <string>
#include <vector>

#include "contract_fixtures.h"
#include "coordstransformer.h"
#include "ddb.h"
#include "dbops.h"
#include "entry.h"
#include "gdal_raii.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "las_test_writer.h"
#include "ogr_spatialref.h"
#include "build.h"
#include "pointcloud.h"
#include "raster_ops.h"
#include "raster_test_utils.h"
#include "real_raster_helper.h"
#include "test.h"
#include "testarea.h"
#include "tile_test_support.h"
#include "thumbs.h"
#include "tilerhelper.h"
#include "tilers_tiler.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    // NAD83(2011) / Arizona Central (ft) with NAVD88 height (ftUS) bound to a geoid grid that is
    // not installed (PROJ4_GRIDS): the CRS of a point cloud of DJI Terra
    const char *const GeoidCompoundWkt =
        R"wkt(COMPD_CS["AZ Central ft + NAVD88 GEOID18",PROJCS["NAD83(2011) / Arizona Central (ft)",)wkt"
        R"wkt(GEOGCS["NAD83(2011)",DATUM["NAD83_National_Spatial_Reference_System_2011",)wkt"
        R"wkt(SPHEROID["GRS 1980",6378137,298.257222101,AUTHORITY["EPSG","7019"]],AUTHORITY["EPSG","1116"]],)wkt"
        R"wkt(PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,)wkt"
        R"wkt(AUTHORITY["EPSG","9122"]],AUTHORITY["EPSG","6318"]],PROJECTION["Transverse_Mercator"],)wkt"
        R"wkt(PARAMETER["latitude_of_origin",31],PARAMETER["central_meridian",-111.916666666667],)wkt"
        R"wkt(PARAMETER["scale_factor",0.9999],PARAMETER["false_easting",700000],)wkt"
        R"wkt(PARAMETER["false_northing",0],UNIT["foot",0.3048,AUTHORITY["EPSG","9002"]],)wkt"
        R"wkt(AXIS["Easting",EAST],AXIS["Northing",NORTH],AUTHORITY["EPSG","6405"]],)wkt"
        R"wkt(VERT_CS["NAVD88 height (ftUS)",VERT_DATUM["North American Vertical Datum 1988",2005,)wkt"
        R"wkt(EXTENSION["PROJ4_GRIDS","us_noaa_g2018u0.tif"]],UNIT["US survey foot",0.304800609601219],)wkt"
        R"wkt(AXIS["Gravity-related height",UP]]])wkt";

    // A site grid: no datum, so no transformation to any geographic CRS
    const char *const EngineeringWkt =
        R"wkt(LOCAL_CS["Site grid",LOCAL_DATUM["Site datum",0],UNIT["metre",1],)wkt"
        R"wkt(AXIS["X",EAST],AXIS["Y",NORTH]])wkt";

    constexpr double UsSurveyFootInMeters = 0.304800609601219;

    struct XyzPoint
    {
        double x, y, z;
    };

    // The LAS file the CRS of a producer would be written in: LAS 1.4, point format 6, the CRS as WKT
    void writeLas(const fs::path &path, const std::string &wkt, const std::vector<XyzPoint> &points)
    {
        std::vector<testutils::LasPoint> las;
        for (const auto &p : points)
            las.push_back({p.x, p.y, p.z});
        // The WKT as a producer that writes it with GDAL stores it (PDAL's LAS writer did)
        OGRSpatialReference srs;
        ASSERT_EQ(srs.importFromWkt(wkt.c_str()), OGRERR_NONE) << wkt;
        char *normalised = nullptr;
        ASSERT_EQ(srs.exportToWkt(&normalised), OGRERR_NONE);
        const std::string stored(normalised);
        CPLFree(normalised);
        testutils::writeLas14(path, stored, las);
    }

    std::string wktOfEpsg(const std::string &userInput)
    {
        const SrsPtr srs = srsFromUserInput(userInput.c_str());
        EXPECT_TRUE(srs) << userInput;
        char *wkt = nullptr;
        srsOf(srs).exportToWkt(&wkt);
        return takeGdalString(wkt);
    }

    // What EPSG:6405 (the horizontal CRS of the point clouds below) says the point is in WGS84
    Point expectedLonLat(double x, double y)
    {
        const SrsPtr from = srsFromEpsg(6405);
        const SrsPtr to = srsFromEpsg(4326);
        CoordsTransformer transform(srsOf(from), srsOf(to));
        transform.transform(&x, &y);
        return Point(x, y, 0.0);
    }

    const std::vector<XyzPoint> ArizonaPoints = {
        {738000.0, 991500.0, 2480.0}, {738100.0, 991550.0, 2490.0}, {738200.0, 991600.0, 2500.0}};

    void expectFootprintOfArizonaPoints(const PointCloudInfo &info)
    {
        EXPECT_TRUE(info.footprintWarning.empty()) << info.footprintWarning;
        ASSERT_EQ(info.polyBounds.size(), 5);
        ASSERT_EQ(info.centroid.size(), 1);
        ASSERT_EQ(info.bounds.size(), 6u);

        const Point southWest = expectedLonLat(738000.0, 991500.0);
        const Point northEast = expectedLonLat(738200.0, 991600.0);
        EXPECT_NEAR(info.polyBounds.points[0].x, southWest.x, 1e-6);
        EXPECT_NEAR(info.polyBounds.points[0].y, southWest.y, 1e-6);
        EXPECT_NEAR(info.polyBounds.points[2].x, northEast.x, 1e-6);
        EXPECT_NEAR(info.polyBounds.points[2].y, northEast.y, 1e-6);

        // The heights are in US survey feet: the footprint carries them in metres, no geoid applied
        EXPECT_NEAR(info.polyBounds.points[0].z, 2480.0 * UsSurveyFootInMeters, 1e-3);
        EXPECT_NEAR(info.centroid.points[0].z, 2480.0 * UsSurveyFootInMeters, 1e-3);
    }

    TEST(PointCloudCrs, VerticalCrsBoundToAMissingGeoidGridKeepsTheFootprint)
    {
        TestArea ta(TEST_NAME);
        const fs::path las = ta.getPath("geoid.las");
        writeLas(las, GeoidCompoundWkt, ArizonaPoints);

        PointCloudInfo info;
        ASSERT_NO_THROW(ASSERT_TRUE(getPointCloudInfo(io::toUtf8(las), info)));
        EXPECT_EQ(info.pointCount, ArizonaPoints.size());
        expectFootprintOfArizonaPoints(info);

        // The entry has a polygon and no warning
        Entry entry;
        ASSERT_NO_THROW(parseEntry(las, ta.getFolder(), entry, false));
        EXPECT_EQ(entry.type, EntryType::PointCloud);
        EXPECT_EQ(entry.polygon_geom.size(), 5);
        EXPECT_FALSE(entry.properties.contains("warning"));
    }

    TEST(PointCloudCrs, CompoundEpsgCodesGiveTheSameFootprint)
    {
        TestArea ta(TEST_NAME);
        const fs::path las = ta.getPath("compound.las");
        writeLas(las, wktOfEpsg("EPSG:6405+6360"), ArizonaPoints);

        PointCloudInfo info;
        ASSERT_TRUE(getPointCloudInfo(io::toUtf8(las), info));
        expectFootprintOfArizonaPoints(info);
    }

    TEST(PointCloudCrs, ProjectedCrsWithoutVerticalPartKeepsHeightsAsTheyAre)
    {
        TestArea ta(TEST_NAME);
        const fs::path las = ta.getPath("projected.las");
        writeLas(las, wktOfEpsg("EPSG:6405"), ArizonaPoints);

        PointCloudInfo info;
        ASSERT_TRUE(getPointCloudInfo(io::toUtf8(las), info));
        ASSERT_EQ(info.polyBounds.size(), 5);
        EXPECT_NEAR(info.polyBounds.points[0].z, 2480.0, 1e-3);
    }

    TEST(PointCloudCrs, CrsWithoutTransformationIsIndexedWithoutFootprintAndWithAWarning)
    {
        TestArea ta(TEST_NAME);
        const fs::path las = ta.getPath("site.las");
        writeLas(las, EngineeringWkt,
                 {{100.0, 200.0, 5.0}, {110.0, 200.0, 6.0}, {110.0, 210.0, 7.0}, {100.0, 210.0, 8.0}});

        PointCloudInfo info;
        ASSERT_NO_THROW(ASSERT_TRUE(getPointCloudInfo(io::toUtf8(las), info)));
        EXPECT_EQ(info.pointCount, 4u);
        EXPECT_TRUE(info.polyBounds.empty());
        EXPECT_TRUE(info.centroid.empty());
        EXPECT_FALSE(info.footprintWarning.empty());
        // The bounds of the file, in its own units, stay: a build needs them
        ASSERT_EQ(info.bounds.size(), 6u);
        EXPECT_NEAR(info.bounds[0], 100.0, 1e-3);
        EXPECT_NEAR(info.bounds[4], 210.0, 1e-3);

        Entry entry;
        ASSERT_NO_THROW(parseEntry(las, ta.getFolder(), entry, false));
        EXPECT_EQ(entry.type, EntryType::PointCloud);
        EXPECT_TRUE(entry.polygon_geom.empty());
        EXPECT_TRUE(entry.point_geom.empty());
        EXPECT_EQ(entry.properties.value("pointCount", 0), 4);
        ASSERT_TRUE(entry.properties.contains("warning"));
        EXPECT_EQ(entry.properties["warning"].get<std::string>().rfind("footprint unavailable: ", 0), 0u);
    }

    TEST(PointCloudCrs, CrsWithoutTransformationBuilds)
    {
        TestArea ta(TEST_NAME);
        const fs::path las = ta.getPath("site-build.las");
        writeLas(las, EngineeringWkt,
                 {{100.0, 200.0, 5.0}, {110.0, 200.0, 6.0}, {110.0, 210.0, 7.0}, {100.0, 210.0, 8.0}});

        const fs::path out = ta.getFolder("copc");
        ASSERT_NO_THROW(buildCopc({io::toUtf8(las)}, io::toUtf8(out)));
        const fs::path copc = out / "cloud.copc.laz";
        ASSERT_TRUE(fs::exists(copc));

        // The COPC file of such a cloud reads back the same way
        PointCloudInfo info;
        ASSERT_TRUE(getCopcInfo(io::toUtf8(copc), info));
        EXPECT_EQ(info.pointCount, 4u);
        EXPECT_EQ(info.bounds.size(), 6u);
        EXPECT_TRUE(info.polyBounds.empty());
        EXPECT_FALSE(info.footprintWarning.empty());
    }

    // A grid of points every 2 ft over 200 ft x 100 ft of Arizona: enough of them for the tiles of
    // the deepest zoom level of its COPC to draw opaque pixels
    std::vector<XyzPoint> arizonaGrid()
    {
        std::vector<XyzPoint> points;
        for (int i = 0; i <= 100; i++)
            for (int j = 0; j <= 50; j++)
                points.push_back({738000.0 + i * 2.0, 991500.0 + j * 2.0, 2480.0 + (i + j) * 0.1});
        return points;
    }

    // The tiles of the coarsest zoom level of the COPC built from a LAS file of points in wkt,
    // decoded, with the number of their opaque pixels
    struct RenderedTiles
    {
        std::vector<testutils::DecodedImage> images;
        size_t opaquePixels = 0;
    };

    RenderedTiles renderCoarsestTiles(TestArea &ta, const std::string &name, const std::string &wkt,
                                      const std::vector<XyzPoint> &points)
    {
        RenderedTiles rendered;
        const fs::path las = ta.getPath(name + ".las");
        writeLas(las, wkt, points);

        const fs::path out = ta.getFolder(name);
        EXPECT_NO_THROW(buildCopc({io::toUtf8(las)}, io::toUtf8(out)));
        const fs::path copc = out / "cloud.copc.laz";
        EXPECT_TRUE(fs::exists(copc));

        // No output folder: the tiles are encoded in memory, into the buffers asked for
        TilersTiler tiler(io::toUtf8(copc), "", 256, false);
        const int minZ = tiler.getMinMaxZ().min;
        for (const auto &t : tiler.getTilesForZoomLevel(minZ))
        {
            uint8_t *buf = nullptr;
            int size = 0;
            tiler.tile(t.tz, t.tx, t.ty, &buf, &size);
            auto image = testutils::decodeImage(testutils::takeBuffer(buf, size));
            EXPECT_TRUE(image.hasAlpha());
            rendered.opaquePixels += static_cast<size_t>(std::count(image.alpha().begin(), image.alpha().end(), 255));
            rendered.images.push_back(std::move(image));
        }
        return rendered;
    }

    TEST(PointCloudCrs, VerticalCrsBoundToAMissingGeoidGridRendersTiles)
    {
        TestArea ta(TEST_NAME);
        const auto points = arizonaGrid();

        RenderedTiles geoid;
        ASSERT_NO_THROW(geoid = renderCoarsestTiles(ta, "geoid", GeoidCompoundWkt, points));
        ASSERT_FALSE(geoid.images.empty());
        EXPECT_GT(geoid.opaquePixels, 100u);

        // The points fall where the horizontal CRS alone puts them: the same tiles as the cloud in
        // EPSG:6405, whatever the vertical datum
        const RenderedTiles horizontal = renderCoarsestTiles(ta, "horizontal", wktOfEpsg("EPSG:6405"), points);
        ASSERT_EQ(geoid.images.size(), horizontal.images.size());
        EXPECT_EQ(geoid.opaquePixels, horizontal.opaquePixels);
        for (size_t i = 0; i < geoid.images.size(); i++)
            EXPECT_EQ(geoid.images[i].bands, horizontal.images[i].bands) << "tile " << i;
    }

    // No tile, but a thumbnail: the cloud drawn in its own coordinates, filling the square
    TEST(PointCloudCrs, CrsWithoutTransformationHasAThumbnailInItsOwnCoordinates)
    {
        TestArea ta(TEST_NAME);
        const fs::path las = ta.getPath("site-thumb.las");
        std::vector<XyzPoint> points;
        for (int i = 0; i <= 40; i++)
            for (int j = 0; j <= 40; j++)
                points.push_back({100.0 + i * 0.25, 200.0 + j * 0.25, 5.0 + i * 0.1});
        writeLas(las, EngineeringWkt, points);
        const fs::path out = ta.getFolder("copc");
        ASSERT_NO_THROW(buildCopc({io::toUtf8(las)}, io::toUtf8(out)));

        uint8_t *buf = nullptr;
        int size = 0;
        ASSERT_NO_THROW(generatePointCloudThumb(out / "cloud.copc.laz", 64, "", &buf, &size));
        const auto image = testutils::decodeImage(testutils::takeBuffer(buf, size));
        EXPECT_EQ(image.width, 64);
        // A fully opaque thumbnail is written without alpha
        const auto opaque = image.hasAlpha() ? std::count(image.alpha().begin(), image.alpha().end(), 255) : 64 * 64;
        EXPECT_GT(opaque, 64 * 64 * 3 / 4);
    }

    TEST(PointCloudCrs, CrsWithoutTransformationRefusesTilesWithTheReason)
    {
        TestArea ta(TEST_NAME);
        const fs::path las = ta.getPath("site-tiles.las");
        writeLas(las, EngineeringWkt,
                 {{100.0, 200.0, 5.0}, {110.0, 200.0, 6.0}, {110.0, 210.0, 7.0}, {100.0, 210.0, 8.0}});

        const fs::path out = ta.getFolder("copc");
        ASSERT_NO_THROW(buildCopc({io::toUtf8(las)}, io::toUtf8(out)));
        const fs::path copc = out / "cloud.copc.laz";

        // The footprint of the tiles is in EPSG:3857: the reason given is the one for that CRS
        PointCloudInfo info;
        ASSERT_TRUE(getCopcInfo(io::toUtf8(copc), info, 3857));
        ASSERT_FALSE(info.footprintWarning.empty());

        uint8_t *buf = nullptr;
        int size = 0;
        try
        {
            TilerHelper::getTile(inMemory(testTile(copc, 20, 0, 0, 256, false, true), &buf, &size));
            FAIL() << "a tile of a point cloud without a footprint";
        }
        catch (const InvalidArgsException &e)
        {
            const std::string message = e.what();
            EXPECT_NE(message.find("footprint unavailable: " + info.footprintWarning), std::string::npos) << message;
        }
        EXPECT_EQ(buf, nullptr);
    }

    // The entry JSON that libddb writes for such a cloud: properties.warning
    TEST(PointCloudCrs, EntryWithoutFootprintMatchesItsContractFixture)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path las = ta.getPath("site-contract.las");
        writeLas(las, EngineeringWkt,
                 {{100.0, 200.0, 5.0}, {110.0, 200.0, 6.0}, {110.0, 210.0, 7.0}, {100.0, 210.0, 8.0}});

        const fs::path root = ta.getFolder("dataset");
        contract::makeDataset(root, {las});

        // The file is not read back from the index: the size of a LAS file depends on how the
        // writer spells its CRS, so it is normalised, and the hash is not asked for
        const std::string path = (root / "site-contract.las").string();
        const char *paths[] = {path.c_str()};
        char *out = nullptr;
        ASSERT_EQ(DDBInfo(paths, 1, &out, "json", false, 0, "auto", false, true), DDBERR_NONE) << DDBGetLastError();
        contract::expectMatchesFixture(
            contract::Fixture{
                "info-pointcloud-no-footprint",
                "A LAS 1.4 point cloud (4 points) whose CRS is a LOCAL_CS site grid, so it has no transformation to "
                "WGS84, copied to <root> with mtime 1700000000",
                "DDBInfo([<root>/site-contract.las], format=\"json\", recursive=false, maxRecursionDepth=0, "
                "geometry=\"auto\", withHash=false, stopOnError=true)",
                "entries.schema.json",
                contract::pathPlaceholders(root, "<root>"),
                {"depth", "size"},
                {}},
            contract::takeJson(out));

        DDBReleaseConnections(root.string().c_str(), nullptr);
    }

    // ---- Point clouds in any CRS

    // A grid of n x n points over `size` degrees around lon, lat, heights from 582 m rising to 730 m
    // across it
    std::vector<testutils::LasPoint> lonLatGrid(double lon, double lat, double size, int n)
    {
        std::vector<testutils::LasPoint> points;
        points.reserve(static_cast<size_t>(n) * n);
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                points.push_back({lon - size / 2 + size * i / (n - 1), lat - size / 2 + size * j / (n - 1),
                                  582.0 + 148.0 * (i + j) / (2.0 * (n - 1))});
        return points;
    }

    // The grid in `crs`, longitude and latitude in steps of 1e-7 degrees: a Metashape export
    void writeLonLatLas(const fs::path &path, const std::string &crs, double lon, double lat, double size, int n)
    {
        testutils::writeLas14(path, wktOfEpsg(crs), lonLatGrid(lon, lat, size, n), testutils::DegreeScale);
    }

    TEST(PointCloudCrs, AGeographicCloudIsIndexedWithTheUtmZoneOfItsCentreForItsCopc)
    {
        TestArea ta(TEST_NAME);
        const fs::path las = ta.getPath("lonlat.las");
        writeLonLatLas(las, "EPSG:4326", 13.84, 46.68, 0.014, 20);

        PointCloudInfo info;
        ASSERT_NO_THROW(ASSERT_TRUE(getPointCloudInfo(io::toUtf8(las), info)));
        EXPECT_EQ(info.crsStatus, PointCloudCrsStatus::Geographic);
        EXPECT_EQ(info.targetCrs, "EPSG:32633");
        EXPECT_TRUE(info.footprintWarning.empty()) << info.footprintWarning;
        ASSERT_EQ(info.polyBounds.size(), 5);
        EXPECT_NEAR(info.polyBounds.points[0].x, 13.833, 1e-3);
        EXPECT_NEAR(info.polyBounds.points[2].y, 46.687, 1e-3);

        Entry entry;
        ASSERT_NO_THROW(parseEntry(las, ta.getFolder(), entry, false));
        EXPECT_EQ(entry.properties["crsStatus"], "geographic");
        EXPECT_FALSE(entry.properties.contains("targetCrs")) << "how the COPC is built is not a property of the file";
        EXPECT_FALSE(entry.properties.contains("warning"));
        EXPECT_EQ(entry.polygon_geom.size(), 5);
    }

    TEST(PointCloudCrs, ACompoundGeographicCloudKeepsItsVerticalCrsForItsCopc)
    {
        TestArea ta(TEST_NAME);
        const fs::path las = ta.getPath("florida.las");
        writeLonLatLas(las, "EPSG:6318+5703", -82.59, 27.53, 0.01, 10);

        PointCloudInfo info;
        ASSERT_TRUE(getPointCloudInfo(io::toUtf8(las), info));
        EXPECT_EQ(info.crsStatus, PointCloudCrsStatus::Geographic);
        EXPECT_EQ(info.targetCrs, "EPSG:6346+5703");
    }

    // Points around lon, lat (a grid of `size` degrees, heights h to h + 20 m), written in EPSG:4978
    struct GeocentricCloud
    {
        std::vector<testutils::LasPoint> points;
        double minLon = 1e300, minLat = 1e300, maxLon = -1e300, maxLat = -1e300;
    };

    GeocentricCloud geocentricCloud(double lon, double lat, double h, double size, int n)
    {
        const SrsPtr from = srsFromEpsg(4979);
        const SrsPtr to = srsFromEpsg(4978);
        CoordsTransformer transform(srsOf(from), srsOf(to));
        GeocentricCloud cloud;
        for (const auto &p : lonLatGrid(lon, lat, size, n))
        {
            double x = p.x, y = p.y, z = h + (p.z - 582.0) / 148.0 * 20.0;
            cloud.minLon = std::min(cloud.minLon, x);
            cloud.maxLon = std::max(cloud.maxLon, x);
            cloud.minLat = std::min(cloud.minLat, y);
            cloud.maxLat = std::max(cloud.maxLat, y);
            EXPECT_TRUE(transform.tryTransform(1, &x, &y, &z));
            cloud.points.push_back({x, y, z});
        }
        return cloud;
    }

    TEST(PointCloudCrs, AGeocentricCloudHasTheFootprintOfItsPointsWithinMetres)
    {
        TestArea ta(TEST_NAME);
        const fs::path las = ta.getPath("ecef.las");
        const GeocentricCloud cloud = geocentricCloud(-83.6, 41.65, 180.0, 0.01, 30);
        testutils::writeLas14(las, wktOfEpsg("EPSG:4978"), cloud.points);

        PointCloudInfo info;
        ASSERT_NO_THROW(ASSERT_TRUE(getPointCloudInfo(io::toUtf8(las), info)));
        EXPECT_EQ(info.crsStatus, PointCloudCrsStatus::Geocentric);
        EXPECT_TRUE(info.footprintWarning.empty()) << info.footprintWarning;
        ASSERT_EQ(info.polyBounds.size(), 5);

        // Within a metre of the envelope of the points (1e-5 degrees): the corners of the box of the
        // points lie 80 m east and west and 60 m north and south of it
        const auto &sw = info.polyBounds.points[0];
        const auto &ne = info.polyBounds.points[2];
        EXPECT_NEAR(sw.x, cloud.minLon, 1e-5);
        EXPECT_NEAR(sw.y, cloud.minLat, 1e-5);
        EXPECT_NEAR(ne.x, cloud.maxLon, 1e-5);
        EXPECT_NEAR(ne.y, cloud.maxLat, 1e-5);
        ASSERT_EQ(info.centroid.size(), 1);
        EXPECT_NEAR(info.centroid.points[0].x, -83.6, 1e-5);
        EXPECT_NEAR(info.centroid.points[0].y, 41.65, 1e-5);
        // The height of the centre of the box: within the 20 m of the points and the bulge of the earth
        EXPECT_NEAR(info.centroid.points[0].z, 190.0, 15.0);

        // Its COPC is in UTM zone 17N with the ellipsoidal height
        const SrsPtr target = srsFromUserInput(info.targetCrs.c_str());
        ASSERT_TRUE(target) << info.targetCrs;
        EXPECT_STREQ(srsOf(target).GetName(), "WGS 84 / UTM zone 17N");
        EXPECT_EQ(srsOf(target).GetAxesCount(), 3);
    }

    // The COPC of a geocentric cloud is in UTM with the ellipsoidal height, every point where it was
    TEST(PointCloudCrs, TheCopcOfAGeocentricCloudIsBuiltInUtmWithTheEllipsoidalHeight)
    {
        TestArea ta(TEST_NAME);
        const fs::path las = ta.getPath("ecef-build.las");
        const GeocentricCloud cloud = geocentricCloud(-83.6, 41.65, 180.0, 0.01, 30);
        testutils::writeLas14(las, wktOfEpsg("EPSG:4978"), cloud.points);
        PointCloudInfo source;
        ASSERT_TRUE(getPointCloudInfo(io::toUtf8(las), source));

        const fs::path out = ta.getFolder("copc");
        CopcBuildRecord record;
        ASSERT_NO_THROW(record = buildCopc({io::toUtf8(las)}, io::toUtf8(out)));
        EXPECT_TRUE(record.transformed);
        EXPECT_TRUE(PointCloudCrsPolicy::isSame(record.copcCrs, source.targetCrs)) << record.copcCrs;

        PointCloudInfo copc;
        ASSERT_TRUE(getCopcInfo(io::toUtf8(out / CopcFileName), copc));
        EXPECT_EQ(copc.pointCount, cloud.points.size());
        EXPECT_EQ(copc.crsStatus, PointCloudCrsStatus::Projected);
        // Heights above the ellipsoid: 180 to 200 m
        EXPECT_NEAR(copc.bounds[2], 180.0, 0.01);
        EXPECT_NEAR(copc.bounds[5], 200.0, 0.01);
        // The footprint of the COPC is where the source is (the envelope of its box in UTM, which
        // the convergence of the meridians turns by 1.7 degrees here: within 50 m)
        ASSERT_EQ(copc.polyBounds.size(), 5);
        EXPECT_NEAR(copc.polyBounds.points[0].x, cloud.minLon, 6e-4);
        EXPECT_NEAR(copc.polyBounds.points[2].y, cloud.maxLat, 4.5e-4);
    }

    // The header of the toledo cloud of the corpus says EPSG:4978, and its points are within 300 m of
    // 0, 0, 0: the CRS is ignored, the entry says so and has no footprint, and its COPC states no CRS
    // (tiles and thumbnail as for a cloud without one)
    TEST(PointCloudCrs, AGeocentricCrsOverLocalCoordinatesIsIgnoredWithAWarning)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path las = ta.getPath("toledo.las");
        std::vector<testutils::LasPoint> points;
        for (int i = 0; i <= 30; i++)
            for (int j = 0; j <= 30; j++)
                points.push_back({-88.0 + i * 10.0, -95.0 + j * 9.5, 1.0 + (i + j) * 0.5});
        testutils::writeLas14(las, wktOfEpsg("EPSG:4978"), points);

        PointCloudInfo info;
        ASSERT_TRUE(getPointCloudInfo(io::toUtf8(las), info));
        EXPECT_EQ(info.crsStatus, PointCloudCrsStatus::Implausible);
        EXPECT_TRUE(info.targetCrs.empty());
        EXPECT_TRUE(info.polyBounds.empty());
        EXPECT_TRUE(info.centroid.empty());

        Entry entry;
        ASSERT_NO_THROW(parseEntry(las, ta.getFolder(), entry, false));
        EXPECT_TRUE(entry.polygon_geom.empty());
        EXPECT_TRUE(entry.point_geom.empty());
        EXPECT_EQ(entry.properties["crsStatus"], "implausible");
        EXPECT_EQ(entry.properties["warning"], "coordinate system ignored: the coordinates do not fit WGS 84");
        EXPECT_FALSE(entry.properties["projection"].get<std::string>().empty()) << "the CRS the file states stays";

        // Built without the CRS: no reader of the COPC places it at the centre of the earth
        const fs::path root = ta.getFolder("dataset");
        const json added = contract::makeDataset(root, {las});
        const std::string r = root.string();
        EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");
        ASSERT_EQ(DDBBuild(r.c_str(), "toledo.las", nullptr, false, false), DDBERR_NONE) << DDBGetLastError();
        bool complete = false;
        ASSERT_EQ(DDBIsBuildComplete(r.c_str(), "toledo.las", &complete), DDBERR_NONE);
        EXPECT_TRUE(complete);

        const fs::path folder = root / ".ddb" / "build" / added[0]["hash"].get<std::string>() / CopcBuildSubfolder;
        const fs::path copc = folder / CopcFileName;
        PointCloudInfo built;
        ASSERT_TRUE(getCopcInfo(io::toUtf8(copc), built));
        EXPECT_EQ(built.wktProjection, "") << "the COPC states no CRS";
        EXPECT_EQ(built.pointCount, points.size());
        EXPECT_NEAR(built.bounds[0], -88.0, 1e-3) << "the points as they are";
        EXPECT_NEAR(built.bounds[4], 190.0, 1e-3);
        const auto record = readCopcBuildRecord(folder / CopcBuildRecordFileName);
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(record->copcCrs, "");
        EXPECT_FALSE(record->transformed);
        EXPECT_FALSE(record->sourceCrs.empty());

        // The manifest names no CRS for it
        const char *paths[] = {"toledo.las"};
        char *out = nullptr;
        ASSERT_EQ(DDBGetBuildArtifacts(r.c_str(), paths, 1, &out), DDBERR_NONE) << DDBGetLastError();
        const json artifacts = contract::takeJson(out);
        const json &artifact = artifacts[0]["build"]["artifacts"][0];
        EXPECT_EQ(artifact["role"], "pointcloud.copc");
        EXPECT_FALSE(artifact.contains("crs")) << artifact.dump();

        // No map tile, as for a cloud without a CRS
        const fs::path plain = ta.getPath("nocrs.las");
        testutils::writeLas14(plain, "", points);
        const fs::path plainOut = ta.getFolder("nocrs");
        ASSERT_NO_THROW(buildCopc({io::toUtf8(plain)}, io::toUtf8(plainOut)));
        for (const fs::path &cloud : {copc, plainOut / CopcFileName})
        {
            uint8_t *buf = nullptr;
            int size = 0;
            EXPECT_THROW(TilerHelper::getTile(inMemory(testTile(cloud, 20, 0, 0, 256, false, true), &buf, &size)),
                         InvalidArgsException)
                << io::toUtf8(cloud);
            EXPECT_EQ(buf, nullptr);
        }

        // A thumbnail in its own coordinates, filling the square
        uint8_t *thumb = nullptr;
        int thumbSize = 0;
        ASSERT_NO_THROW(generatePointCloudThumb(copc, 64, "", &thumb, &thumbSize));
        const auto image = testutils::decodeImage(testutils::takeBuffer(thumb, thumbSize));
        EXPECT_EQ(image.width, 64);
        const auto opaque = image.hasAlpha() ? std::count(image.alpha().begin(), image.alpha().end(), 255) : 64 * 64;
        EXPECT_GT(opaque, 64 * 64 / 2);
        DDBReleaseConnections(r.c_str(), nullptr);
    }

    TEST(PointCloudCrs, AGeographicCrsBeyondThePolesIsIgnoredWithAWarning)
    {
        TestArea ta(TEST_NAME);
        const fs::path las = ta.getPath("beyond.las");
        testutils::writeLas14(las, wktOfEpsg("EPSG:4326"), {{13.8, 46.6, 500.0}, {13.9, 95.0, 600.0}});

        Entry entry;
        ASSERT_NO_THROW(parseEntry(las, ta.getFolder(), entry, false));
        EXPECT_EQ(entry.properties["crsStatus"], "implausible");
        EXPECT_EQ(entry.properties["warning"], "coordinate system ignored: the coordinates do not fit WGS 84");
        EXPECT_TRUE(entry.polygon_geom.empty());
    }

    // A cloud in US feet (Autzen: NAD83(HARN) / Oregon GIC Lambert, international feet) keeps its CRS
    TEST(PointCloudCrs, AProjectedCloudInFeetKeepsItsCrs)
    {
        TestArea ta(TEST_NAME);
        const fs::path las = ta.getPath("autzen.las");
        std::vector<testutils::LasPoint> points;
        for (int i = 0; i <= 20; i++)
            for (int j = 0; j <= 20; j++)
                points.push_back({635577.0 + i * 100.0, 848882.0 + j * 100.0, 406.0 + i});
        testutils::writeLas14(las, wktOfEpsg("EPSG:2994"), points);

        PointCloudInfo info;
        ASSERT_TRUE(getPointCloudInfo(io::toUtf8(las), info));
        EXPECT_EQ(info.crsStatus, PointCloudCrsStatus::Projected);
        EXPECT_TRUE(info.targetCrs.empty());
        EXPECT_EQ(info.polyBounds.size(), 5);

        const fs::path out = ta.getFolder("copc");
        CopcBuildRecord record;
        ASSERT_NO_THROW(record = buildCopc({io::toUtf8(las)}, io::toUtf8(out)));
        EXPECT_FALSE(record.transformed);
        EXPECT_EQ(record.crsPolicy, 1);
        EXPECT_TRUE(PointCloudCrsPolicy::isSame(record.copcCrs, "EPSG:2994")) << record.copcCrs;
        EXPECT_TRUE(PointCloudCrsPolicy::isSame(record.sourceCrs, "EPSG:2994"));
    }

    // ---- The COPC of a geographic cloud

    // A cloud of 0.014 x 0.014 degrees and 148 m of height in EPSG:4326 (the meadows export of the
    // release benchmark, smaller): a cube sized by its heights in degrees would hold it in a sliver
    constexpr int GeographicGrid = 700;

    TEST(PointCloudCrs, TheCopcOfAGeographicCloudIsBuiltInUtmWithAnOctreeThatSplitsIt)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path las = ta.getPath("meadows.las");
        writeLonLatLas(las, "EPSG:4326", 13.84, 46.68, 0.014, GeographicGrid);
        const size_t count = static_cast<size_t>(GeographicGrid) * GeographicGrid;

        const fs::path out = ta.getFolder("copc");
        CopcBuildRecord record;
        ASSERT_NO_THROW(record = buildCopc({io::toUtf8(las)}, io::toUtf8(out))) << DDBGetLastError();
        EXPECT_TRUE(record.transformed);
        EXPECT_TRUE(PointCloudCrsPolicy::isSame(record.sourceCrs, "EPSG:4326"));
        EXPECT_TRUE(PointCloudCrsPolicy::isSame(record.copcCrs, "EPSG:32633")) << record.copcCrs;

        const fs::path copc = out / CopcFileName;
        PointCloudInfo info;
        ASSERT_TRUE(getCopcInfo(io::toUtf8(copc), info));
        EXPECT_EQ(info.pointCount, count);
        EXPECT_TRUE(PointCloudCrsPolicy::isSame(info.wktProjection, "EPSG:32633"));
        // Metres of UTM 33N: about 1,070 m by 1,560 m
        EXPECT_NEAR(info.bounds[3] - info.bounds[0], 1070.0, 30.0);
        EXPECT_NEAR(info.bounds[4] - info.bounds[1], 1557.0, 30.0);
        EXPECT_NEAR(info.bounds[2], 582.0, 0.01);
        EXPECT_NEAR(info.bounds[5], 730.0, 0.01);
        // Its footprint is where the cloud is
        ASSERT_EQ(info.polyBounds.size(), 5);
        EXPECT_NEAR(info.polyBounds.points[0].x, 13.833, 1e-3);
        EXPECT_NEAR(info.polyBounds.points[0].y, 46.673, 1e-3);

        // The octree splits the cloud: its largest node holds about 40,000 points, where a cube in
        // degrees (148 "degrees" wide for 0.014 degrees of cloud) keeps 107,000 in one node of this
        // cloud, and millions in one of the 160 M points of the meadows export
        raster::CopcParams params;
        params.threads = 4;
        params.targetCrs = "EPSG:32633";
        const fs::path again = ta.getFolder("again") / CopcFileName;
        const raster::CopcResult result = raster::buildCopcFile(las, again, params, {});
        EXPECT_TRUE(result.transformed);
        ASSERT_TRUE(result.maxNodePoints.has_value());
        EXPECT_LE(*result.maxNodePoints, 80000u);

        // A tile of the deepest level draws the points, quickly
        TilersTiler tiler(io::toUtf8(copc), "", 256, false);
        const int maxZ = tiler.getMinMaxZ().max;
        const auto tiles = tiler.getTilesForZoomLevel(maxZ);
        ASSERT_FALSE(tiles.empty());
        const auto &middle = tiles[tiles.size() / 2];
        uint8_t *buf = nullptr;
        int size = 0;
        const auto start = std::chrono::steady_clock::now();
        tiler.tile(middle.tz, middle.tx, middle.ty, &buf, &size);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000);
        const auto image = testutils::decodeImage(testutils::takeBuffer(buf, size));
        EXPECT_GT(std::count(image.alpha().begin(), image.alpha().end(), 255), 1000);
    }

    // The build of a geographic entry records its CRSs beside the COPC, which completes it; a COPC
    // without the record (built before the CRS policy) is built again by the next build
    TEST(PointCloudCrs, AGeographicEntryBuiltBeforeTheCrsPolicyIsBuiltAgain)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path las = ta.getPath("lonlat.las");
        writeLonLatLas(las, "EPSG:4326", 13.84, 46.68, 0.014, 100);
        const fs::path root = ta.getFolder("dataset");
        const json added = contract::makeDataset(root, {las});
        const std::string r = root.string();
        EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");

        ASSERT_EQ(DDBBuild(r.c_str(), "lonlat.las", nullptr, false, false), DDBERR_NONE) << DDBGetLastError();
        bool complete = false;
        ASSERT_EQ(DDBIsBuildComplete(r.c_str(), "lonlat.las", &complete), DDBERR_NONE);
        EXPECT_TRUE(complete);

        const std::string hash = added[0]["hash"].get<std::string>();
        const fs::path folder = root / ".ddb" / "build" / hash / CopcBuildSubfolder;
        const auto record = readCopcBuildRecord(folder / CopcBuildRecordFileName);
        ASSERT_TRUE(record.has_value());
        EXPECT_TRUE(record->transformed);
        EXPECT_TRUE(PointCloudCrsPolicy::isSame(record->copcCrs, "EPSG:32633"));

        // The manifest says in which CRS the COPC is
        const char *paths[] = {"lonlat.las"};
        char *out = nullptr;
        ASSERT_EQ(DDBGetBuildArtifacts(r.c_str(), paths, 1, &out), DDBERR_NONE) << DDBGetLastError();
        const json artifacts = contract::takeJson(out);
        const json &copc = artifacts[0]["build"]["artifacts"][0];
        EXPECT_EQ(copc["role"], "pointcloud.copc");
        EXPECT_EQ(copc["crs"], (json{{"name", "WGS 84 / UTM zone 33N"}, {"epsg", 32633}, {"transformed", true}}));

        // A COPC of a build before the policy: the same file, no record
        fs::remove(folder / CopcBuildRecordFileName);
        ASSERT_EQ(DDBIsBuildComplete(r.c_str(), "lonlat.las", &complete), DDBERR_NONE);
        EXPECT_FALSE(complete);
        ASSERT_EQ(DDBBuild(r.c_str(), "lonlat.las", nullptr, false, false), DDBERR_NONE) << DDBGetLastError();
        ASSERT_EQ(DDBIsBuildComplete(r.c_str(), "lonlat.las", &complete), DDBERR_NONE);
        EXPECT_TRUE(complete);
        EXPECT_TRUE(fs::exists(folder / CopcBuildRecordFileName));
        DDBReleaseConnections(r.c_str(), nullptr);
    }

    // A dataset upgraded in place from 2.7: its index has the properties 2.7 wrote (a projection, no
    // crsStatus) and its build the COPC 2.7 built in degrees, without a build record. Every API that
    // reports the state of a build (the manifest, which Registry's completeness sweep reads, the
    // listing with builds, the status) says it is not complete, so that it is built again, and the
    // manifest names the CRS of the COPC it has
    TEST(PointCloudCrs, AGeographicCopcOfAnIndexOf27IsNotCompleteForEveryApi)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path las = ta.getPath("meadows-geo.las");
        writeLonLatLas(las, "EPSG:4326", 13.84, 46.68, 0.014, 60);
        const fs::path root = ta.getFolder("dataset");
        const json added = contract::makeDataset(root, {las});
        const std::string r = root.string();
        const std::string hash = added[0]["hash"].get<std::string>();

        {
            const auto db = ddb::open(r, false);
            auto q = db->query("UPDATE entries SET properties = json_remove(properties, '$.crsStatus') WHERE path = ?");
            q->bind(1, "meadows-geo.las");
            q->execute();
        }
        const fs::path folder = root / ".ddb" / "build" / hash / CopcBuildSubfolder;
        fs::create_directories(folder);
        raster::CopcParams params;
        params.threads = 2;
        raster::buildCopcFile(las, folder / CopcFileName, params, {});
        ASSERT_FALSE(fs::exists(folder / CopcBuildRecordFileName));

        const char *paths[] = {"meadows-geo.las"};
        char *out = nullptr;
        ASSERT_EQ(DDBGetBuildArtifacts(r.c_str(), paths, 1, &out), DDBERR_NONE) << DDBGetLastError();
        const json artifacts = contract::takeJson(out);
        const json &build = artifacts[0]["build"];
        EXPECT_EQ(build["state"], "none") << build.dump();
        ASSERT_EQ(build["artifacts"].size(), 1u) << build.dump();
        EXPECT_EQ(build["artifacts"][0]["crs"], (json{{"name", "WGS 84"}, {"epsg", 4326}, {"transformed", false}}))
            << build.dump();

        DDBListEntriesOptions options{sizeof(DDBListEntriesOptions), true, false, true, 0};
        ASSERT_EQ(DDBListEntriesEx(r.c_str(), "", "", "", &options, &out), DDBERR_NONE) << DDBGetLastError();
        const json listed = contract::takeJson(out);
        ASSERT_EQ(listed.size(), 1u);
        EXPECT_EQ(listed[0]["build"]["state"], "none") << listed.dump();
        EXPECT_EQ(listed[0]["build"]["artifacts"][0].value("crs", json()), build["artifacts"][0]["crs"]) << listed.dump();

        bool complete = true;
        ASSERT_EQ(DDBIsBuildComplete(r.c_str(), "meadows-geo.las", &complete), DDBERR_NONE);
        EXPECT_FALSE(complete);
        ASSERT_EQ(DDBGetBuildStatus(r.c_str(), paths, 1, &out), DDBERR_NONE) << DDBGetLastError();
        const json status = contract::takeJson(out);
        EXPECT_EQ(status[0]["complete"], false) << status.dump();
        DDBReleaseConnections(r.c_str(), nullptr);
    }

    // The real program, counting the reads of a cloud's info and the COPC builds
    class CountingCloudHelper : public testutils::RealRasterHelper
    {
      public:
        PointCloudInfo pointCloudInfo(const fs::path &path) override
        {
            infos++;
            return RealRasterHelper::pointCloudInfo(path);
        }
        json buildCopc(const fs::path &path, const fs::path &out, const json &params,
                       const raster::ProgressCallback &progress) override
        {
            builds++;
            return RealRasterHelper::buildCopc(path, out, params, progress);
        }

        std::atomic<int> infos{0};
        std::atomic<int> builds{0};
    };

    // A dataset of one cloud, indexed, and what its build recorded
    struct IndexedCloud
    {
        fs::path root;
        std::string hash;

        std::optional<CopcBuildRecord> record() const
        {
            return readCopcBuildRecord(root / ".ddb" / "build" / hash / CopcBuildSubfolder / CopcBuildRecordFileName);
        }
    };

    IndexedCloud indexCloud(TestArea &ta, const std::string &name, const std::function<void(const fs::path &)> &write)
    {
        const fs::path las = ta.getPath(name);
        write(las);
        const fs::path root = ta.getFolder("dataset-" + name);
        const json added = contract::makeDataset(root, {las});
        return {root, added[0]["hash"].get<std::string>()};
    }

    // The index of 2.8 has what the CRS policy needs of a cloud (its status, its CRS, its footprint):
    // the build of its entry asks the helper for the COPC only, whatever the status
    TEST(PointCloudCrs, AnEntryIndexedWithItsCrsStatusBuildsWithTheCopcRequestOnly)
    {
        TestArea ta(TEST_NAME, true);
        EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");
        const IndexedCloud geographic = indexCloud(ta, "geo.las", [](const fs::path &p) {
            writeLonLatLas(p, "EPSG:4326", 13.84, 46.68, 0.014, 40);
        });
        const IndexedCloud projected = indexCloud(ta, "utm.las", [](const fs::path &p) {
            testutils::writeLas14(p, wktOfEpsg("EPSG:32633"), {{410000, 5170000, 580}, {410500, 5170600, 600}});
        });
        const IndexedCloud implausible = indexCloud(ta, "site.las", [](const fs::path &p) {
            testutils::writeLas14(p, wktOfEpsg("EPSG:4978"), {{-88, -95, 1}, {212, 190, 40}, {50, 60, 20}});
        });

        CountingCloudHelper helper;
        const raster::ScopedRasterHelper installed(helper);
        for (const auto *cloud : {&geographic, &projected, &implausible})
        {
            const std::string r = cloud->root.string();
            ASSERT_EQ(DDBBuild(r.c_str(), nullptr, nullptr, false, false), DDBERR_NONE) << DDBGetLastError();
            DDBReleaseConnections(r.c_str(), nullptr);
        }
        EXPECT_EQ(helper.infos, 0) << "the index has what the policy needs";
        EXPECT_EQ(helper.builds, 3);

        // The same COPCs as when the helper reads the clouds
        const auto geoRecord = geographic.record();
        ASSERT_TRUE(geoRecord.has_value());
        EXPECT_TRUE(geoRecord->transformed);
        EXPECT_TRUE(PointCloudCrsPolicy::isSame(geoRecord->copcCrs, "EPSG:32633")) << geoRecord->copcCrs;
        const auto utmRecord = projected.record();
        ASSERT_TRUE(utmRecord.has_value());
        EXPECT_FALSE(utmRecord->transformed);
        EXPECT_TRUE(PointCloudCrsPolicy::isSame(utmRecord->copcCrs, "EPSG:32633"));
        const auto siteRecord = implausible.record();
        ASSERT_TRUE(siteRecord.has_value());
        EXPECT_EQ(siteRecord->copcCrs, "");

        // A single entry, forced
        const std::string r = geographic.root.string();
        ASSERT_EQ(DDBBuild(r.c_str(), "geo.las", nullptr, true, false), DDBERR_NONE) << DDBGetLastError();
        EXPECT_EQ(helper.infos, 0);
        EXPECT_EQ(helper.builds, 4);
        DDBReleaseConnections(r.c_str(), nullptr);
    }

    // An entry indexed before the status was recorded, and a cloud that is no entry, are read by the
    // helper first
    TEST(PointCloudCrs, ACloudWithoutAnIndexedStatusIsReadByTheHelperBeforeItsBuild)
    {
        TestArea ta(TEST_NAME, true);
        EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");
        const IndexedCloud before = indexCloud(ta, "geo27.las", [](const fs::path &p) {
            writeLonLatLas(p, "EPSG:4326", 13.84, 46.68, 0.014, 40);
        });
        {
            const auto db = ddb::open(before.root.string(), false);
            auto q = db->query("UPDATE entries SET properties = json_remove(properties, '$.crsStatus')");
            q->execute();
        }

        CountingCloudHelper helper;
        const raster::ScopedRasterHelper installed(helper);
        const std::string r = before.root.string();
        ASSERT_EQ(DDBBuild(r.c_str(), "geo27.las", nullptr, false, false), DDBERR_NONE) << DDBGetLastError();
        DDBReleaseConnections(r.c_str(), nullptr);
        EXPECT_EQ(helper.infos, 1);
        EXPECT_EQ(helper.builds, 1);
        const auto record = before.record();
        ASSERT_TRUE(record.has_value());
        EXPECT_TRUE(record->transformed);

        const fs::path out = ta.getFolder("loose");
        CopcBuildRecord loose;
        ASSERT_NO_THROW(loose = buildCopc({io::toUtf8(before.root / "geo27.las")}, io::toUtf8(out)));
        EXPECT_TRUE(loose.transformed);
        EXPECT_EQ(helper.infos, 2);
        EXPECT_EQ(helper.builds, 2);
    }

    // The Florida file of the corpus (4.7 M points, NAD83(2011) + NAVD88, longitude and latitude):
    // its COPC is in NAD83(2011) / UTM zone 17N + NAVD88 height, with every point
    MANUAL_TEST(PointCloudCrs, TheFloridaCloudIsBuiltInUtmOnItsDatum)
    {
        const fs::path las = "/mnt/data/datasets/point-clouds/test-crash/20210126_342500e_3046000n.laz";
        if (!fs::exists(las))
            GTEST_SKIP() << las << " is not on this machine";
        TestArea ta(TEST_NAME, true);

        PointCloudInfo info;
        ASSERT_TRUE(getPointCloudInfo(io::toUtf8(las), info));
        EXPECT_EQ(info.crsStatus, PointCloudCrsStatus::Geographic);
        EXPECT_EQ(info.targetCrs, "EPSG:6346+5703");

        const fs::path out = ta.getFolder("copc");
        CopcBuildRecord record;
        ASSERT_NO_THROW(record = buildCopc({io::toUtf8(las)}, io::toUtf8(out)));
        EXPECT_TRUE(record.transformed);
        PointCloudInfo copc;
        ASSERT_TRUE(getCopcInfo(io::toUtf8(out / CopcFileName), copc));
        EXPECT_EQ(copc.pointCount, info.pointCount);
        EXPECT_TRUE(PointCloudCrsPolicy::isSame(copc.wktProjection, "EPSG:6346+5703")) << copc.wktProjection;
    }
}
