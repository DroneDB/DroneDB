/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// CAD drawings (DDB-12): DWG support via LibreDWG dwg2dxf; CAD CRS from the
// drawing's GEODATA or an optional same-name .prj; drawings without a CRS
// build in local coordinates (shown without a globe); honest failures; no
// orphan-.prj builds.
//
// Assets: test_data/vector/autocad/ (see LIBREDWG-ASSETS.md there). Set
// DDB_CAD_TEST_ASSETS=<folder> to use a local copy (no download). Tests that
// need the real converter skip unless dwg2dxf is discoverable
// (DDB_DWG2DXF_PATH or next to the test executable), like the Obj2Tiles
// end-to-end tests.

#include "build.h"
#include "cad.h"
#include "dbops.h"
#include "ddb.h"
#include "exceptions.h"
#include "gdal_failure_counter.h"
#include "gdal_priv.h"
#include "mio.h"
#include "raster_cache.h"
#include "test.h"
#include "testarea.h"
#include "vector.h"
#include "vector_query.h"

#include <chrono>
#include <cstdlib>
#include <fstream>

namespace
{

    using namespace ddb;

    const char *kAssetBaseUrl =
        "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/autocad/";

    fs::path cadAsset(TestArea &ta, const std::string &name)
    {
        if (const char *local = std::getenv("DDB_CAD_TEST_ASSETS"))
        {
            const fs::path src = fs::path(local) / name;
            const fs::path dst = ta.getPath(name);
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
            return dst;
        }
        return ta.downloadTestAsset(std::string(kAssetBaseUrl) + name, name);
    }

    bool converterAvailable()
    {
        return !cad::findDwg2DxfBinary().empty();
    }

    void setEnv(const char *name, const std::string &value)
    {
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

    void unsetEnv(const char *name)
    {
#ifdef _WIN32
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }

#define REQUIRE_CONVERTER()                                                    \
    do                                                                         \
    {                                                                          \
        if (!converterAvailable())                                             \
            GTEST_SKIP() << "dwg2dxf not available (set DDB_DWG2DXF_PATH)";    \
    } while (0)

    struct GpkgSummary
    {
        bool opened = false;
        long long features = 0;
        OGREnvelope extent;
    };

    GpkgSummary readGpkg(const fs::path &baseDir)
    {
        GpkgSummary s;
        const auto gpkg = baseDir / "vec" / "source.gpkg";
        if (!fs::exists(gpkg))
            return s;
        GDALDatasetH ds = GDALOpenEx(gpkg.string().c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                     nullptr, nullptr, nullptr);
        if (!ds)
            return s;
        s.opened = true;
        bool first = true;
        for (int i = 0; i < GDALDatasetGetLayerCount(ds); i++)
        {
            OGRLayerH l = GDALDatasetGetLayer(ds, i);
            s.features += OGR_L_GetFeatureCount(l, TRUE);
            OGREnvelope e;
            if (OGR_L_GetExtent(l, &e, TRUE) != OGRERR_NONE)
                continue;
            if (first)
                s.extent = e;
            else
                s.extent.Merge(e);
            first = false;
        }
        GDALClose(ds);
        return s;
    }

    bool gpkgHasSrs(const fs::path &baseDir)
    {
        GDALDatasetH ds = GDALOpenEx((baseDir / "vec" / "source.gpkg").string().c_str(),
                                     GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (!ds)
            return false;
        bool any = false;
        for (int i = 0; i < GDALDatasetGetLayerCount(ds); i++)
            any = any || OGR_L_GetSpatialRef(GDALDatasetGetLayer(ds, i)) != nullptr;
        GDALClose(ds);
        return any;
    }

    void expectInside(const OGREnvelope &e, double minX, double maxX, double minY, double maxY)
    {
        EXPECT_GE(e.MinX, minX);
        EXPECT_LE(e.MaxX, maxX);
        EXPECT_GE(e.MinY, minY);
        EXPECT_LE(e.MaxY, maxY);
    }

    // Milan, EPSG:32632 site in utm32n_site.dxf reprojected with its .prj
    // (measured with ogr2ogr: 9.179067..9.185477 E, 45.459376..45.463886 N).
    void expectMilanSite(const OGREnvelope &e)
    {
        expectInside(e, 9.17, 9.19, 45.45, 45.47);
    }

    std::string buildError(const fs::path &input, const fs::path &out)
    {
        try
        {
            buildVector(input.string(), out.string());
        }
        catch (const AppException &e)
        {
            return e.what();
        }
        return "";
    }

    // ---------------------------------------------------------------- DWG

    // A modern (R2018) DWG builds like any other vector (the bundled GDAL has
    // no DWG reader: the build goes through dwg2dxf).
    TEST(cadVector, DwgBuildsVectorArtifacts)
    {
        REQUIRE_CONVERTER();
        TestArea ta(TEST_NAME, true);
        const auto dwg = cadAsset(ta, "example_2018.dwg");
        cadAsset(ta, "example_2018.prj");
        const auto out = ta.getPath("build");

        {
            // A DWG block with a hatch is a GEOMETRYCOLLECTION holding a
            // MULTISURFACE: the MVT writer rejected it tile by tile (and the
            // Windows test process died there) until it was normalized.
            GdalFailureCounter failures;
            ASSERT_NO_THROW(buildVector(dwg.string(), out.string()));
            EXPECT_EQ(failures.value(), 0) << "GDAL reported failures while building";
        }
        const auto s = readGpkg(out);
        ASSERT_TRUE(s.opened);
        // 125 = what stock GDAL reads from LibreDWG 0.14's DXF (127 with the
        // fit-point SPLINE patch, see DwgFitPointSplinesSurvive).
        EXPECT_GE(s.features, 125);
        expectInside(s.extent, -180, 180, -90, 90);
        EXPECT_TRUE(fs::exists(out / "mvt" / "metadata.json"));
        // The intermediate DXF never lands in the output.
        for (const auto &f : fs::recursive_directory_iterator(out))
            EXPECT_NE(f.path().extension(), ".dxf") << f.path();
    }

    // Parity with AutoCAD's own DXF of the same drawing (127 features).
    // LibreDWG writes fit-point-only SPLINEs, which stock GDAL drops (2 of 127);
    // vcpkg-overlays/gdal makes the DXF driver read them.
    TEST(cadVector, DwgFitPointSplinesSurvive)
    {
        REQUIRE_CONVERTER();
        TestArea ta(TEST_NAME, true);
        const auto dwg = cadAsset(ta, "example_2018.dwg");
        cadAsset(ta, "example_2018.prj");
        const auto ref = cadAsset(ta, "example_2018.autocad.dxf");
        cadAsset(ta, "example_2018.autocad.prj");

        buildVector(dwg.string(), ta.getPath("dwg").string());
        buildVector(ref.string(), ta.getPath("ref").string());
        const auto a = readGpkg(ta.getPath("dwg"));
        const auto b = readGpkg(ta.getPath("ref"));
        ASSERT_TRUE(a.opened && b.opened);
        EXPECT_EQ(b.features, 127);
        EXPECT_EQ(a.features, b.features);
    }

    // dwg2dxf exits 0 on this half file but writes an unusable DXF. The build
    // must fail and say why, and leave no artifacts.
    TEST(cadVector, TruncatedDwgFailsWithActionableMessage)
    {
        REQUIRE_CONVERTER();
        TestArea ta(TEST_NAME, true);
        const auto dwg = cadAsset(ta, "example_2007_truncated.dwg");
        const auto out = ta.getPath("build");

        const auto msg = buildError(dwg, out);
        ASSERT_FALSE(msg.empty()) << "a truncated DWG must not build";
        EXPECT_NE(msg.find("DWG"), std::string::npos) << msg;
        EXPECT_EQ(msg.find("Cannot open vector source"), std::string::npos) << msg;
        EXPECT_FALSE(fs::exists(out / "vec" / "source.gpkg"));
    }

    TEST(cadVector, NotADwgFailsWithActionableMessage)
    {
        REQUIRE_CONVERTER();
        TestArea ta(TEST_NAME, true);
        const auto dwg = cadAsset(ta, "not_a_dwg.dwg");

        const auto msg = buildError(dwg, ta.getPath("build"));
        ASSERT_FALSE(msg.empty());
        EXPECT_NE(msg.find("not a valid DWG"), std::string::npos) << msg;
    }

    // Without the converter the error names the tool and the override.
    TEST(cadVector, MissingConverterIsActionable)
    {
        TestArea ta(TEST_NAME, true);
        const auto dwg = cadAsset(ta, "example_2018.dwg");
        cadAsset(ta, "example_2018.prj");

        const char *prev = std::getenv("DDB_DWG2DXF_PATH");
        const std::string saved = prev ? prev : "";
        setEnv("DDB_DWG2DXF_PATH", ta.getPath("missing/dwg2dxf").string());
        const auto msg = buildError(dwg, ta.getPath("build"));
        if (prev)
            setEnv("DDB_DWG2DXF_PATH", saved);
        else
            unsetEnv("DDB_DWG2DXF_PATH");

        EXPECT_NE(msg.find("dwg2dxf"), std::string::npos) << msg;
        EXPECT_NE(msg.find("DDB_DWG2DXF_PATH"), std::string::npos) << msg;
    }

    // ---------------------------------------------------------------- CRS

    // GDAL's DXF driver ignores a same-name .prj; DroneDB reads it (the ArcGIS
    // convention for CAD files) instead of taking the UTM metres as degrees.
    TEST(cadVector, CadPrjSidecarSetsSourceCrs)
    {
        TestArea ta(TEST_NAME, true);
        const auto dxf = cadAsset(ta, "utm32n_site.dxf");
        cadAsset(ta, "utm32n_site.prj");
        const auto out = ta.getPath("build");

        buildVector(dxf.string(), out.string());
        const auto s = readGpkg(out);
        ASSERT_TRUE(s.opened);
        EXPECT_EQ(s.features, 4);
        expectMilanSite(s.extent);
    }

    TEST(cadVector, DwgPrjSidecarSetsSourceCrs)
    {
        REQUIRE_CONVERTER();
        TestArea ta(TEST_NAME, true);
        const auto dwg = cadAsset(ta, "utm32n_site_dwg.dwg");
        cadAsset(ta, "utm32n_site_dwg.prj");
        const auto out = ta.getPath("build");

        buildVector(dwg.string(), out.string());
        const auto s = readGpkg(out);
        ASSERT_TRUE(s.opened);
        EXPECT_EQ(s.features, 4);
        expectMilanSite(s.extent);
    }

    // No CRS (no GEODATA, no .prj) and coordinates that cannot be degrees: the
    // drawing builds in its own coordinates, like a local Obj2Tiles model. The
    // GPKG has no SRS and no MVT pyramid is made (nothing to put on a globe).
    TEST(cadVector, CadWithoutCrsBuildsLocal)
    {
        TestArea ta(TEST_NAME, true);
        const auto dxf = cadAsset(ta, "utm32n_site.dxf");
        const auto out = ta.getPath("build");

        ASSERT_NO_THROW(buildVector(dxf.string(), out.string()));
        const auto s = readGpkg(out);
        ASSERT_TRUE(s.opened);
        EXPECT_EQ(s.features, 4);
        expectInside(s.extent, 514000 - 1e-6, 514500 + 1e-6, 5034000 - 1e-6, 5034500 + 1e-6);
        EXPECT_FALSE(gpkgHasSrs(out));
        EXPECT_FALSE(fs::exists(out / "mvt"));
    }

    // The Hub's local (no globe) viewer reads the features of a local drawing
    // through OGC API Features -> queryVector: they must come back in drawing
    // coordinates instead of failing on the missing SRS.
    TEST(cadVector, LocalGpkgIsQueryableInDrawingCoordinates)
    {
        TestArea ta(TEST_NAME, true);
        const auto dxf = cadAsset(ta, "utm32n_site.dxf");
        const auto out = ta.getPath("build");
        buildVector(dxf.string(), out.string());

        const auto geojson = json::parse(queryVector((out / "vec" / "source.gpkg").string(), "", nullptr, "",
                                                     1000, 0, "geojson"));
        ASSERT_EQ(geojson["features"].size(), 4u);
        bool sawPoint = false;
        for (const auto &f : geojson["features"])
            if (f["geometry"]["type"] == "MultiPoint")
            {
                sawPoint = true;
                EXPECT_NEAR(f["geometry"]["coordinates"][0][0].get<double>(), 514100.0, 1e-6);
                EXPECT_NEAR(f["geometry"]["coordinates"][0][1].get<double>(), 5034100.0, 1e-6);
            }
        EXPECT_TRUE(sawPoint);
    }

    // AutoCAD/Civil 3D store the drawing's coordinate system in a GEODATA
    // object; dwg2dxf keeps it in the DXF, GDAL ignores it. Coordinate type 2
    // + EPSG alias -> that CRS, no .prj needed.
    TEST(cadVector, CadGeodataSetsCrs)
    {
        TestArea ta(TEST_NAME, true);
        const auto dxf = cadAsset(ta, "utm32n_site_geodata.dxf");
        const auto out = ta.getPath("build");

        buildVector(dxf.string(), out.string());
        const auto s = readGpkg(out);
        ASSERT_TRUE(s.opened);
        EXPECT_EQ(s.features, 4);
        expectMilanSite(s.extent);
        EXPECT_TRUE(fs::exists(out / "mvt" / "metadata.json"));
    }

    // GEODATA type 1 (local grid): the design point of the drawing is the reference
    // point of the coordinate system (DDB-12 P4). The site drawing, its GEODATA turned
    // into a local grid, lands where the projected grid put it, or 1 km east of it
    // when its design point moves 1 km west.
    TEST(cadVector, CadGeodataLocalGridPlacesTheDrawing)
    {
        TestArea ta(TEST_NAME, true);
        const auto grid = cadAsset(ta, "utm32n_site_geodata.dxf");
        std::ifstream in(grid.string(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string geoData = "AcDbGeoData\n 90\n2\n330\n0\n 70\n2\n 10\n514000.0\n";
        ASSERT_NE(text.find(geoData), std::string::npos);

        const auto variant = [&](const std::string &name, const std::string &designX) {
            std::string t = text;
            t.replace(t.find(geoData), geoData.size(),
                      "AcDbGeoData\n 90\n2\n330\n0\n 70\n1\n 10\n" + designX + "\n");
            const auto path = ta.getPath(name);
            std::ofstream(path.string(), std::ios::binary) << t;
            return path;
        };

        buildVector(grid.string(), ta.getPath("grid").string());
        buildVector(variant("same.dxf", "514000.0").string(), ta.getPath("same").string());
        buildVector(variant("west.dxf", "513000.0").string(), ta.getPath("west").string());
        const auto g = readGpkg(ta.getPath("grid"));
        const auto same = readGpkg(ta.getPath("same"));
        const auto east = readGpkg(ta.getPath("west"));
        ASSERT_TRUE(g.opened && same.opened && east.opened);

        EXPECT_NEAR(same.extent.MinX, g.extent.MinX, 1e-9);
        EXPECT_NEAR(same.extent.MaxY, g.extent.MaxY, 1e-9);
        const double kmInDegrees = 1000.0 / (111320.0 * std::cos(45.46 * M_PI / 180.0));
        EXPECT_NEAR(east.extent.MinX - g.extent.MinX, kmInDegrees, 2e-4);
        EXPECT_NEAR(east.extent.MinY, g.extent.MinY, 2e-4);
    }

    // Decision D1: CAD files without a CRS whose coordinates fit in lon/lat
    // keep the historical EPSG:4326 assumption (all five older DXF test assets
    // do; civil_war_by_campaign.dxf is real lon/lat).
    TEST(cadVector, CadWithoutCrsInsideLonLatStillBuilds)
    {
        TestArea ta(TEST_NAME, true);
        const auto dxf = ta.downloadTestAsset(std::string(kAssetBaseUrl) + "autocad.dxf", "autocad.dxf");
        const auto out = ta.getPath("build");

        ASSERT_NO_THROW(buildVector(dxf.string(), out.string()));
        const auto s = readGpkg(out);
        ASSERT_TRUE(s.opened);
        EXPECT_GT(s.features, 0);
    }

    // D1: the lon/lat assumption is for DXF only. A DWG is written by CAD software,
    // which stores GEODATA when the drawing is georeferenced; a DWG without CRS
    // that fits in +-180/+-90 is a design drawing (all 6 such samples were).
    TEST(cadVector, DwgWithoutCrsInsideLonLatBuildsLocal)
    {
        REQUIRE_CONVERTER();
        TestArea ta(TEST_NAME, true);
        const auto dwg = cadAsset(ta, "small_drawing_dwg.dwg");
        const auto out = ta.getPath("build");

        ASSERT_NO_THROW(buildVector(dwg.string(), out.string()));
        EXPECT_TRUE(fs::exists(out / "vec" / "local.json"));
        EXPECT_FALSE(fs::exists(out / "mvt"));
        EXPECT_FALSE(gpkgHasSrs(out));
    }

    // The same drawing as DXF keeps the assumption (GIS tools export lon/lat as DXF).
    TEST(cadVector, DxfWithoutCrsInsideLonLatAssumesWgs84)
    {
        TestArea ta(TEST_NAME, true);
        const auto dxf = cadAsset(ta, "small_drawing.dxf");
        const auto out = ta.getPath("build");

        buildVector(dxf.string(), out.string());
        EXPECT_TRUE(fs::exists(out / "mvt" / "metadata.json"));
        EXPECT_TRUE(gpkgHasSrs(out));
    }

    // A .prj with a local (engineering) CRS keeps a drawing off the map on purpose,
    // overriding the lon/lat assumption.
    TEST(cadVector, LocalPrjForcesLocal)
    {
        TestArea ta(TEST_NAME, true);
        const auto dxf = cadAsset(ta, "small_drawing_local.dxf");
        cadAsset(ta, "small_drawing_local.prj");
        const auto out = ta.getPath("build");

        buildVector(dxf.string(), out.string());
        EXPECT_TRUE(fs::exists(out / "vec" / "local.json"));
        EXPECT_FALSE(fs::exists(out / "mvt"));
    }

    // ------------------------------------------------------- index / sidecars

    class CadIndex : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            // Unique area per run: initIndex refuses an existing .ddb.
            ta = std::make_unique<TestArea>(
                TEST_NAME + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            root = ta->getPath("");
            ddb::initIndex(root.string());
            db = ddb::open(root.string(), true);
        }
        void TearDown() override
        {
            db.reset();
            ta.reset();
        }
        fs::path buildDirOf(const std::string &path)
        {
            Entry e;
            EXPECT_TRUE(ddb::getEntry(db.get(), path, e));
            return db->buildDirectory() / e.hash;
        }

        std::unique_ptr<TestArea> ta;
        fs::path root;
        std::unique_ptr<Database> db;
    };

    // The CRS rule also decides the index-time footprint the Hub uses to pick
    // map/globe vs local viewer (a DXF without CRS must not get its metres
    // stored as a "WGS84" polygon).
    TEST_F(CadIndex, CadIndexFootprintFollowsCrs)
    {
        const auto geo = cadAsset(*ta, "utm32n_site_geodata.dxf");
        const auto local = cadAsset(*ta, "utm32n_site.dxf");
        ddb::addToIndex(db.get(), {geo.string(), local.string()});

        Entry g, l;
        ASSERT_TRUE(ddb::getEntry(db.get(), "utm32n_site_geodata.dxf", g));
        ASSERT_TRUE(ddb::getEntry(db.get(), "utm32n_site.dxf", l));

        ASSERT_FALSE(g.polygon_geom.empty());
        for (int i = 0; i < g.polygon_geom.size(); i++)
        {
            const auto pt = g.polygon_geom.getPoint(i);
            EXPECT_TRUE(pt.x > 9.17 && pt.x < 9.19 && pt.y > 45.45 && pt.y < 45.47)
                << pt.x << "," << pt.y;
        }
        EXPECT_NE(g.properties.value("georeferenced", true), false);

        EXPECT_TRUE(l.polygon_geom.empty());
        EXPECT_EQ(l.properties.value("georeferenced", true), false);
    }

    // The index records where the CRS came from, for the Hub (assumed WGS84 is
    // shown as such).
    TEST_F(CadIndex, CrsSourceRecordedForAssumedAndLocalPrj)
    {
        const auto assumed = cadAsset(*ta, "small_drawing.dxf");
        const auto forced = cadAsset(*ta, "small_drawing_local.dxf");
        const auto prj = cadAsset(*ta, "small_drawing_local.prj");
        ddb::addToIndex(db.get(), {assumed.string(), forced.string(), prj.string()});

        Entry a, f;
        ASSERT_TRUE(ddb::getEntry(db.get(), "small_drawing.dxf", a));
        ASSERT_TRUE(ddb::getEntry(db.get(), "small_drawing_local.dxf", f));
        EXPECT_EQ(a.properties["vector"]["crs"].value("source", ""), "lonlat");
        EXPECT_EQ(a.properties.value("georeferenced", false), true);
        EXPECT_FALSE(a.polygon_geom.empty());

        EXPECT_EQ(f.properties["vector"]["crs"].value("source", ""), "prj");
        EXPECT_EQ(f.properties["vector"]["crs"].value("label", ""), "local");
        EXPECT_EQ(f.properties.value("georeferenced", true), false);
        EXPECT_TRUE(f.polygon_geom.empty());
    }

    // The .prj is optional, but adding one later re-parses the drawing and
    // rebuilds it with that CRS (the sidecar mechanism of .dbf/.shx).
    TEST_F(CadIndex, PrjSidecarRebuildsCadEntry)
    {
        const auto dxf = cadAsset(*ta, "utm32n_site.dxf");
        ddb::addToIndex(db.get(), {dxf.string()});
        ddb::build(db.get(), "utm32n_site.dxf", "");
        // Local first: complete without an MVT pyramid.
        EXPECT_TRUE(isBuildComplete(db.get(), "utm32n_site.dxf"));
        EXPECT_TRUE(fs::exists(buildDirOf("utm32n_site.dxf") / "vec" / "local.json"));

        const auto prj = cadAsset(*ta, "utm32n_site.prj");
        ddb::addToIndex(db.get(), {prj.string()});
        std::string sub;
        ASSERT_TRUE(isBuildable(db.get(), "utm32n_site.prj", sub));
        ddb::build(db.get(), "utm32n_site.prj", "");

        EXPECT_TRUE(isBuildComplete(db.get(), "utm32n_site.dxf"));
        const auto s = readGpkg(buildDirOf("utm32n_site.dxf"));
        ASSERT_TRUE(s.opened);
        expectMilanSite(s.extent);

        // The main entry is re-parsed: it now has a WGS84 footprint.
        Entry e;
        ASSERT_TRUE(ddb::getEntry(db.get(), "utm32n_site.dxf", e));
        EXPECT_NE(e.properties.value("georeferenced", true), false);
        ASSERT_FALSE(e.polygon_geom.empty());
        EXPECT_GT(e.polygon_geom.getPoint(0).x, 9.17);
        EXPECT_LT(e.polygon_geom.getPoint(0).x, 9.19);
    }

    // A .prj with no .shp/.dxf/.dwg next to it is not buildable (otherwise it
    // is never "complete" and completeness sweeps re-enqueue it forever).
    TEST_F(CadIndex, OrphanPrjIsNotBuildable)
    {
        const auto prj = ta->getPath("orphan.prj");
        std::ofstream(prj) << "GEOGCS[\"GCS_WGS_1984\"]";
        ddb::addToIndex(db.get(), {prj.string()});

        std::string sub;
        EXPECT_FALSE(isBuildable(db.get(), "orphan.prj", sub));
    }

    // Removing the .prj reverts the drawing to local: footprint dropped, old
    // georeferenced artifacts retired, and the next build is local again.
    TEST_F(CadIndex, RemovingPrjRevertsToLocal)
    {
        const auto dxf = cadAsset(*ta, "utm32n_site.dxf");
        const auto prj = cadAsset(*ta, "utm32n_site.prj");
        ddb::addToIndex(db.get(), {dxf.string(), prj.string()});
        ddb::build(db.get(), "utm32n_site.dxf", "");
        ASSERT_TRUE(fs::exists(buildDirOf("utm32n_site.dxf") / "mvt" / "metadata.json"));

        // Registry order: remove from the index first, delete the file after. The
        // re-read in between must already ignore the removed .prj.
        ddb::removeFromIndex(db.get(), {prj.string()});
        Entry e;
        ASSERT_TRUE(ddb::getEntry(db.get(), "utm32n_site.dxf", e));
        EXPECT_TRUE(e.polygon_geom.empty());
        EXPECT_EQ(e.properties.value("georeferenced", true), false);
        EXPECT_FALSE(isBuildComplete(db.get(), "utm32n_site.dxf"));
        fs::remove(prj);

        ddb::build(db.get(), "utm32n_site.dxf", "");
        EXPECT_TRUE(isBuildComplete(db.get(), "utm32n_site.dxf"));
        EXPECT_TRUE(fs::exists(buildDirOf("utm32n_site.dxf") / "vec" / "local.json"));
        EXPECT_FALSE(fs::exists(buildDirOf("utm32n_site.dxf") / "mvt"));
    }

    // A GeoPackage left open by a WFS/OGC query does not make the .prj removal
    // fail on Windows: the stale artifacts are released, then removed.
    TEST_F(CadIndex, RemovingPrjReleasesCachedVectorHandles)
    {
        const auto dxf = cadAsset(*ta, "utm32n_site.dxf");
        const auto prj = cadAsset(*ta, "utm32n_site.prj");
        ddb::addToIndex(db.get(), {dxf.string(), prj.string()});
        ddb::build(db.get(), "utm32n_site.dxf", "");
        const auto gpkg = buildDirOf("utm32n_site.dxf") / "vec" / "source.gpkg";
        ASSERT_TRUE(fs::exists(gpkg));

        RasterCache::instance().release(gpkg);
        RasterCache::instance().acquireVectorDataset(gpkg.string()); // given back idle
        ASSERT_GE(RasterCache::instance().stats().idle, 1u);

        EXPECT_NO_THROW(ddb::removeFromIndex(db.get(), {prj.string()}));
        EXPECT_FALSE(fs::exists(gpkg.parent_path()));
    }

    // The same rule for shapefile sidecars: no .shp, nothing to build.
    TEST_F(CadIndex, OrphanDbfIsNotBuildable)
    {
        const auto dbf = ta->getPath("lonely.dbf");
        std::ofstream(dbf) << "x";
        ddb::addToIndex(db.get(), {dbf.string()});

        std::string sub;
        EXPECT_FALSE(isBuildable(db.get(), "lonely.dbf", sub));
        EXPECT_FALSE(isBuildComplete(db.get(), "lonely.dbf"));
    }

    // ------------------------------------------------------------ cad.h units

    TEST(cadGeoData, ReadsTypeAndDefinitionFromDxf)
    {
        TestArea ta(TEST_NAME, true);
        const auto info = cad::readDxfInfo(cadAsset(ta, "utm32n_site_geodata.dxf"));
        EXPECT_EQ(info.geoDataType, 2);
        EXPECT_NE(info.geoDataDefinition.find("<Alias id=\"32632\" type=\"CoordinateSystem\">"), std::string::npos);
        EXPECT_EQ(info.insUnits, 6);
        EXPECT_EQ(cad::insUnitsName(info.insUnits), "m");

        const auto plain = cad::readDxfInfo(cadAsset(ta, "utm32n_site.dxf"));
        EXPECT_EQ(plain.geoDataType, -1);
        EXPECT_TRUE(plain.geoDataDefinition.empty());
    }

    TEST(cadGeoData, EpsgAliasOnlyForGridTypes)
    {
        const std::string xml =
            "<Dictionary><Alias id=\"1565\" type=\"Transformation\"><ObjectId>T</ObjectId>"
            "<Namespace>EPSG Code</Namespace></Alias><Alias id=\"2193\" type=\"CoordinateSystem\">"
            "<ObjectId>NZTM</ObjectId><Namespace>EPSG Code</Namespace></Alias></Dictionary>";
        EXPECT_EQ(cad::srsFromGeoData(2, xml).value_or(""), "EPSG:2193");
        EXPECT_EQ(cad::srsFromGeoData(3, xml).value_or(""), "EPSG:2193");
        // Unset (0) and local grid with reference point (1) have no usable CRS.
        EXPECT_FALSE(cad::srsFromGeoData(0, xml).has_value());
        EXPECT_FALSE(cad::srsFromGeoData(1, xml).has_value());
        // AutoCAD's default, never-configured GEODATA.
        EXPECT_FALSE(cad::srsFromGeoData(0, "").has_value());
        EXPECT_FALSE(cad::srsFromGeoData(2, "<Dictionary/>").has_value());
        // A WKT definition (older writers) is accepted as is.
        EXPECT_TRUE(cad::srsFromGeoData(2, "EPSG:32632").has_value());
    }

    TEST(cadGeoData, LocalGridIsAnAffineOfItsCoordinateSystem)
    {
        cad::DxfInfo info;
        info.geoDataType = 1;
        info.geoDataVersion = 2;
        info.geoDataDefinition = "<Dictionary><Alias id=\"32632\" type=\"CoordinateSystem\">"
                                 "<ObjectId>UTM84-32N</ObjectId><Namespace>EPSG Code</Namespace></Alias></Dictionary>";
        info.geoDesignX = 100;
        info.geoDesignY = 200;
        info.geoReferenceX = 514000;
        info.geoReferenceY = 5034000;
        const double phi = 30 * M_PI / 180;
        info.geoNorthX = std::sin(phi); // the drawing's north, 30 degrees clockwise of its Y axis
        info.geoNorthY = std::cos(phi);
        info.geoUnitScale = 0.001; // millimetres

        const auto wkt = cad::srsFromGeoData(info);
        ASSERT_TRUE(wkt.has_value());
        OGRSpatialReference local, utm;
        ASSERT_EQ(local.SetFromUserInput(wkt->c_str()), OGRERR_NONE);
        utm.importFromEPSG(32632);
        local.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        utm.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&local, &utm));
        ASSERT_TRUE(ct);

        double x = 100, y = 200;
        ASSERT_TRUE(ct->Transform(1, &x, &y));
        EXPECT_NEAR(x, 514000, 1e-6) << "the design point is the reference point";
        EXPECT_NEAR(y, 5034000, 1e-6);

        // 1000 mm along the drawing's north is 1 m grid north
        x = 100 + 1000 * std::sin(phi);
        y = 200 + 1000 * std::cos(phi);
        ASSERT_TRUE(ct->Transform(1, &x, &y));
        EXPECT_NEAR(x, 514000, 1e-6);
        EXPECT_NEAR(y, 5034001, 1e-6);

        // Not readable: version 1 layout, no projected coordinate system
        auto old = info;
        old.geoDataVersion = 1;
        EXPECT_FALSE(cad::srsFromGeoData(old).has_value());
        auto geographic = info;
        geographic.geoDataDefinition = "EPSG:4326";
        EXPECT_FALSE(cad::srsFromGeoData(geographic).has_value());
        auto noScale = info;
        noScale.geoUnitScale = 0;
        EXPECT_FALSE(cad::srsFromGeoData(noScale).has_value());
    }

    // The Hub's "Set coordinate system" writes this as the drawing's .prj
    TEST(cadPrj, WrittenPrjReadsBackAsThatCrs)
    {
        TestArea ta(TEST_NAME, true);
        const auto dxf = cadAsset(ta, "utm32n_site.dxf");

        const auto prj = cad::prjContent("EPSG:32632");
        EXPECT_NE(prj.find("PROJCS[\"WGS_1984_UTM_Zone_32N\""), std::string::npos) << "ESRI WKT";
        std::ofstream(ta.getPath("utm32n_site.prj").string(), std::ios::binary) << prj;

        const auto out = ta.getPath("build");
        buildVector(dxf.string(), out.string());
        const auto s = readGpkg(out);
        ASSERT_TRUE(s.opened);
        expectMilanSite(s.extent);

        // "local" keeps the drawing in its own coordinates
        EXPECT_EQ(cad::prjContent("local"), "LOCAL_CS[\"Drawing\"]");
        EXPECT_EQ(cad::prjContent(" local "), "LOCAL_CS[\"Drawing\"]");
        EXPECT_THROW(cad::prjContent("EPSG:999999"), InvalidArgsException);
        EXPECT_THROW(cad::prjContent(""), InvalidArgsException);

        char *out2 = nullptr;
        ASSERT_EQ(DDBMakeCadPrj("EPSG:2193", &out2), DDBERR_NONE);
        EXPECT_NE(std::string(out2).find("NZGD_2000"), std::string::npos);
        DDBFree(out2);
        EXPECT_NE(DDBMakeCadPrj("not a crs", &out2), DDBERR_NONE);
    }

    TEST(cadGeoData, DwgVersionFromMagic)
    {
        TestArea ta(TEST_NAME, true);
        EXPECT_EQ(cad::dwgVersion(cadAsset(ta, "example_2018.dwg")), "AC1032 (R2018)");
        EXPECT_EQ(cad::dwgVersion(cadAsset(ta, "utm32n_site_dwg.dwg")), "AC1015 (R2000)");
        EXPECT_THROW(cad::dwgVersion(cadAsset(ta, "not_a_dwg.dwg")), AppException);
    }

    // A DWG is indexed through the converter too; without one the entry is
    // still indexed (no geometry) and carries the reason.
    TEST_F(CadIndex, DwgWithoutConverterIsIndexedWithWarning)
    {
        const auto dwg = cadAsset(*ta, "utm32n_site_dwg.dwg");
        const char *prev = std::getenv("DDB_DWG2DXF_PATH");
        const std::string saved = prev ? prev : "";
        setEnv("DDB_DWG2DXF_PATH", ta->getPath("missing/dwg2dxf").string());
        ddb::addToIndex(db.get(), {dwg.string()});
        if (prev)
            setEnv("DDB_DWG2DXF_PATH", saved);
        else
            unsetEnv("DDB_DWG2DXF_PATH");

        Entry e;
        ASSERT_TRUE(ddb::getEntry(db.get(), "utm32n_site_dwg.dwg", e));
        EXPECT_EQ(e.type, EntryType::Vector);
        EXPECT_TRUE(e.polygon_geom.empty());
        EXPECT_NE(e.properties.value("warning", "").find("dwg2dxf"), std::string::npos) << e.properties.dump();
    }

    TEST_F(CadIndex, DwgIndexedLocalAndWithPrj)
    {
        if (!converterAvailable())
            GTEST_SKIP() << "dwg2dxf not available (set DDB_DWG2DXF_PATH)";
        const auto dwg = cadAsset(*ta, "utm32n_site_dwg.dwg");
        ddb::addToIndex(db.get(), {dwg.string()});
        Entry e;
        ASSERT_TRUE(ddb::getEntry(db.get(), "utm32n_site_dwg.dwg", e));
        EXPECT_EQ(e.properties.value("georeferenced", true), false);
        EXPECT_EQ(e.properties["vector"].value("converter", ""), "dwg2dxf");

        const auto prj = cadAsset(*ta, "utm32n_site_dwg.prj");
        ddb::addToIndex(db.get(), {prj.string()});
        ASSERT_TRUE(ddb::getEntry(db.get(), "utm32n_site_dwg.dwg", e));
        EXPECT_NE(e.properties.value("georeferenced", true), false);
        ASSERT_FALSE(e.polygon_geom.empty());
        EXPECT_GT(e.polygon_geom.getPoint(0).x, 9.17);
        EXPECT_EQ(e.properties["vector"]["crs"].value("source", ""), "prj");
    }

} // namespace
