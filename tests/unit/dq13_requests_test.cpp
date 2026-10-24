/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-13: the request structs that replaced long parameter lists keep the defaults of the parameters
// they replaced, the library no longer prints the thumbnails it makes, and an index row parses
// through Entry::Row as it did through ten parameters.

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "3d.h"
#include "buildlod_runner.h"
#include "delta_apply.h"
#include "entry.h"
#include "gtest/gtest.h"
#include "info.h"
#include "mio.h"
#include "raster_region.h"
#include "raster_test_utils.h"
#include "shareservice.h"
#include "stac.h"
#include "test.h"
#include "testarea.h"
#include "thumbs.h"
#include "tilerhelper.h"

namespace
{
    using namespace ddb;

    TEST(Dq13Requests, TheStructsKeepTheDefaultsOfTheParametersTheyReplaced)
    {
        const TileSource source;
        EXPECT_EQ(source.tileSize, 256);
        EXPECT_FALSE(source.tms);
        EXPECT_FALSE(source.forceRecreate);
        EXPECT_TRUE(source.pathHash.empty());

        const TileRequest tile;
        EXPECT_EQ(tile.format, "png");
        EXPECT_FALSE(tile.visParams.has_value());
        EXPECT_EQ(tile.outBuffer, nullptr);

        const TilerRun run;
        EXPECT_EQ(run.tileSize, 256);
        EXPECT_EQ(run.format, OutputFormat::Text);
        EXPECT_EQ(run.zRange, "auto");
        EXPECT_EQ(run.x, "auto");
        EXPECT_EQ(run.y, "auto");

        const InfoOptions info;
        EXPECT_EQ(info.format, OutputFormat::Text);
        EXPECT_FALSE(info.recursive);
        EXPECT_EQ(info.maxRecursionDepth, 0);
        EXPECT_EQ(info.geometry, "auto");
        EXPECT_FALSE(info.withHash);
        EXPECT_TRUE(info.stopOnError);

        const StacItemQuery stac;
        EXPECT_TRUE(stac.bbox.empty());
        EXPECT_EQ(stac.limit, 10);
        EXPECT_EQ(stac.offset, 0);

        const Tiles3DOptions tiles;
        EXPECT_FALSE(tiles.overwrite);
        EXPECT_FALSE(tiles.georef.has_value());
        EXPECT_TRUE(tiles.autoDetectGeoref);

        const ShareRequest share;
        EXPECT_TRUE(share.sslVerify);
        EXPECT_EQ(share.callback, nullptr);

        const buildlod::BuildLodRequest lod;
        EXPECT_TRUE(lod.quality);
        EXPECT_EQ(lod.maxSh, 1);

        const DeltaApplication application;
        EXPECT_TRUE(application.verifyHashes);
        EXPECT_EQ(application.mergeStrategy, DontMerge);
        EXPECT_TRUE(application.sourceMetaDump.is_array());

        const RegionOptions region;
        EXPECT_TRUE(region.outputCrs.empty());
        EXPECT_TRUE(region.bands.empty());
    }

    // Writing a thumbnail is not printing it: the CLI prints what the library reports
    TEST(Dq13Requests, GenerateThumbsReportsTheThumbnailsAndPrintsNothing)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 64;
        const auto raster = testutils::createGeoRaster(ta.getPath("ortho.tif"), spec);

        std::ostringstream captured;
        std::streambuf *previous = std::cout.rdbuf(captured.rdbuf());
        std::vector<fs::path> reported;
        try
        {
            generateThumbs({io::toUtf8(raster)}, ta.getFolder("thumbs"), 32, false,
                           [&](const fs::path &thumb) { reported.push_back(thumb); });
        }
        catch (...)
        {
            std::cout.rdbuf(previous);
            throw;
        }
        std::cout.rdbuf(previous);

        EXPECT_TRUE(captured.str().empty()) << captured.str();
        ASSERT_EQ(reported.size(), 1u);
        EXPECT_TRUE(fs::exists(reported[0]));
        EXPECT_EQ(reported[0].extension(), ".webp");

        // Without a callback it just writes
        std::ostringstream again;
        previous = std::cout.rdbuf(again.rdbuf());
        generateThumbs({io::toUtf8(raster)}, ta.getFolder("thumbs2"), 32, false);
        std::cout.rdbuf(previous);
        EXPECT_TRUE(again.str().empty());
        EXPECT_TRUE(fs::exists(ta.getPath("thumbs2") / "ortho.webp"));
    }

    TEST(Dq13Requests, AnIndexRowParsesIntoAnEntry)
    {
        Entry::Row row;
        row.path = "a/b.jpg";
        row.hash = "abc";
        row.type = static_cast<int>(EntryType::GeoImage);
        row.propertiesJson = R"({"width":4,"height":3})";
        row.mtime = 1234;
        row.size = 99;
        row.depth = 1;
        row.pointCoordinatesJson = "[10.5,20.5,30]";
        row.polyCoordinatesJson = "[[[0,0],[1,0],[1,1],[0,0]]]";
        row.metaJson = R"({"title":{"id":"x","data":"t","mtime":1}})";

        Entry e;
        e.parseFields(row);
        EXPECT_EQ(e.path, "a/b.jpg");
        EXPECT_EQ(e.hash, "abc");
        EXPECT_EQ(e.type, EntryType::GeoImage);
        EXPECT_EQ(e.properties["width"], 4);
        EXPECT_EQ(e.mtime, 1234);
        EXPECT_EQ(e.size, 99u);
        EXPECT_EQ(e.depth, 1);
        EXPECT_FALSE(e.point_geom.empty());
        EXPECT_EQ(e.polygon_geom.size(), 4u);
        EXPECT_TRUE(e.meta.contains("title"));

        // The levels a row does not carry stay empty
        Entry summary;
        summary.parseFields(Entry::Row{"p", "h", static_cast<int>(EntryType::Generic), "{}", 1, 2, 0, "", "", ""});
        EXPECT_TRUE(summary.point_geom.empty());
        EXPECT_TRUE(summary.polygon_geom.empty());
    }
}
