/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-09: the list of tiles `ddb tile` prints, as text lines or as a JSON array.

#include <set>
#include <sstream>

#include "gtest/gtest.h"

#include "gdal_inc.h"
#include "gdal_priv.h"
#include "gdal_raii.h"
#include "ogr_spatialref.h"
#include "mio.h"
#include "tilerhelper.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    // A 64x64 3-band byte GeoTIFF in EPSG:3857, 10 m pixels: it spans several zoom levels
    fs::path makeRaster(TestArea &ta)
    {
        const auto tif = ta.getPath("raster.tif");
        double gt[6] = {1000000.0, 10.0, 0.0, 5000640.0, 0.0, -10.0};
        GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
        auto ds = GDALDatasetUniquePtr(driver->Create(tif.string().c_str(), 64, 64, 3, GDT_Byte, nullptr));
        EXPECT_TRUE(ds != nullptr);
        ds->SetGeoTransform(gt);
        OGRSpatialReference srs;
        srs.importFromEPSG(3857);
        char *wkt = nullptr;
        srs.exportToWkt(&wkt);
        ds->SetProjection(wkt);
        CPLFree(wkt);
        for (int b = 1; b <= 3; b++)
            EXPECT_EQ(ds->GetRasterBand(b)->Fill(100.0 + b), CE_None);
        EXPECT_EQ(closeDataset(std::move(ds)), CE_None);
        return tif;
    }

    std::set<std::string> lines(const std::string &text)
    {
        std::set<std::string> result;
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);)
            result.insert(line);
        return result;
    }

    std::set<std::string> strings(const json &array)
    {
        std::set<std::string> result;
        for (const auto &item : array)
            result.insert(item.get<std::string>());
        return result;
    }

    // The JSON list of tiles of several zoom levels used to run the levels together:
    // ["a","b""c","d"] (no comma where a zoom level ends)
    TEST(Dq09TilerOutput, TheJsonTileListOfSeveralZoomLevelsIsAValidArray)
    {
        TestArea ta(TEST_NAME, true);
        const auto tif = makeRaster(ta);

        std::ostringstream asText, asJson;
        TilerHelper::runTiler(tif, ta.getFolder("text"), 256, false, asText, OutputFormat::Text, "14-17");
        TilerHelper::runTiler(tif, ta.getFolder("json"), 256, false, asJson, OutputFormat::Json, "14-17");

        const auto textTiles = lines(asText.str());
        ASSERT_GT(textTiles.size(), 3u);

        const json tiles = json::parse(asJson.str());
        ASSERT_TRUE(tiles.is_array());
        EXPECT_EQ(tiles.size(), textTiles.size());

        for (const auto &tile : strings(tiles))
            EXPECT_TRUE(fs::exists(io::fromUtf8(tile))) << tile;
    }

    TEST(Dq09TilerOutput, TheJsonOfASingleTileIsAOneStringArray)
    {
        TestArea ta(TEST_NAME, true);
        const auto tif = makeRaster(ta);

        std::ostringstream all;
        TilerHelper::runTiler(tif, ta.getFolder("all"), 256, false, all, OutputFormat::Text, "16");
        const std::string first = *lines(all.str()).begin();
        // <folder>/<z>/<x>/<y>.png, with the platform's separators
        const fs::path tile = io::fromUtf8(first);
        ASSERT_FALSE(tile.parent_path().empty()) << first;
        const std::string x = io::toUtf8(tile.parent_path().filename());
        const std::string y = io::toUtf8(tile.stem());

        std::ostringstream out;
        TilerHelper::runTiler(tif, ta.getFolder("single"), 256, false, out, OutputFormat::Json, "16", x, y);
        const json tiles = json::parse(out.str());
        ASSERT_EQ(tiles.size(), 1u);
        EXPECT_TRUE(tiles[0].is_string());
    }

    TEST(Dq09TilerOutput, TextIsOneTilePerLine)
    {
        TestArea ta(TEST_NAME, true);
        const auto tif = makeRaster(ta);

        std::ostringstream out;
        TilerHelper::runTiler(tif, ta.getFolder("tiles"), 256, false, out, OutputFormat::Text, "16");
        const auto tiles = lines(out.str());
        EXPECT_GE(tiles.size(), 1u);
        for (const auto &t : tiles)
            EXPECT_TRUE(fs::exists(io::fromUtf8(t))) << t;
    }
}
