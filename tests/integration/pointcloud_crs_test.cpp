/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Point clouds whose coordinate reference system cannot be used as a whole: a compound CRS whose
// vertical part is bound to a geoid grid that is not installed (DJI Terra writes them), and a CRS
// that has no transformation to WGS84. The footprint is computed from the horizontal CRS, and when it
// cannot be, the file is still indexed and built, without a footprint and with a warning.

#include <algorithm>
#include <string>
#include <vector>

#include "contract_fixtures.h"
#include "coordstransformer.h"
#include "ddb.h"
#include "entry.h"
#include "gdal_raii.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "pdal_inc.h"
#include <pdal/PointTable.hpp>
#include <pdal/io/BufferReader.hpp>
#include <pdal/io/LasWriter.hpp>
#include "pctiler.h"
#include "pointcloud.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "tile_test_support.h"
#include "tilerhelper.h"
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
        pdal::PointTable table;
        table.layout()->registerDim(pdal::Dimension::Id::X);
        table.layout()->registerDim(pdal::Dimension::Id::Y);
        table.layout()->registerDim(pdal::Dimension::Id::Z);

        pdal::PointViewPtr view(new pdal::PointView(table));
        for (size_t i = 0; i < points.size(); i++)
        {
            view->setField(pdal::Dimension::Id::X, i, points[i].x);
            view->setField(pdal::Dimension::Id::Y, i, points[i].y);
            view->setField(pdal::Dimension::Id::Z, i, points[i].z);
        }

        pdal::BufferReader reader;
        reader.addView(view);

        pdal::Options options;
        options.add("filename", io::toUtf8(path));
        options.add("a_srs", wkt);
        options.add("minor_version", 4);
        options.add("dataformat_id", 6);
        options.add("scale_x", 0.001);
        options.add("scale_y", 0.001);
        options.add("scale_z", 0.001);
        options.add("offset_x", "auto");
        options.add("offset_y", "auto");
        options.add("offset_z", "auto");
        // The bytes of the file do not depend on the day or the PDAL version that wrote them
        options.add("creation_year", 2024);
        options.add("creation_doy", 1);
        options.add("system_id", "ddb tests");
        options.add("software_id", "ddb tests");

        pdal::LasWriter writer;
        writer.setOptions(options);
        writer.setInput(reader);
        writer.prepare(table);
        writer.execute(table);
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
        PointCloudTiler tiler(io::toUtf8(copc), "", 256, false);
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
}
