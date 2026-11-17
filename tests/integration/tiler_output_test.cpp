/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The list of tiles `ddb tile` prints, as text lines or as a JSON array.

#include <array>
#include <set>
#include <sstream>

#include "gtest/gtest.h"

#include "gdal_inc.h"
#include "gdal_priv.h"
#include "gdal_raii.h"
#include "ogr_spatialref.h"
#include "mio.h"
#include "raster_tiler.h"
#include "tile_test_support.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    // A GeoTIFF of w x h pixels in EPSG:3857 with the geotransform gt: three byte bands of one colour and,
    // with `alpha`, an alpha band, opaque everywhere, which makes the pixels its COG adds around it transparent
    fs::path writeRaster(TestArea &ta, const std::string &name, const std::array<double, 6> &gt, int w, int h,
                         bool alpha)
    {
        const auto tif = ta.getPath(name);
        const int bands = alpha ? 4 : 3;
        GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
        auto ds = GDALDatasetUniquePtr(driver->Create(tif.string().c_str(), w, h, bands, GDT_Byte, nullptr));
        EXPECT_TRUE(ds != nullptr);
        std::array<double, 6> geoTransform = gt;
        ds->SetGeoTransform(geoTransform.data());
        OGRSpatialReference srs;
        srs.importFromEPSG(3857);
        char *wkt = nullptr;
        srs.exportToWkt(&wkt);
        ds->SetProjection(wkt);
        CPLFree(wkt);
        for (int b = 1; b <= 3; b++)
            EXPECT_EQ(ds->GetRasterBand(b)->Fill(100.0 + b), CE_None);
        if (alpha)
        {
            EXPECT_EQ(ds->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand), CE_None);
            EXPECT_EQ(ds->GetRasterBand(4)->Fill(255.0), CE_None);
        }
        EXPECT_EQ(closeDataset(std::move(ds)), CE_None);
        return tif;
    }

    // A 64x64 3-band byte GeoTIFF in EPSG:3857, 10 m pixels: it spans several zoom levels
    fs::path makeRaster(TestArea &ta)
    {
        return writeRaster(ta, "raster.tif", {1000000.0, 10.0, 0.0, 5000640.0, 0.0, -10.0}, 64, 64, false);
    }

    // The tile (z, x, y) of a path <folder>/<z>/<x>/<y>.png that runTiler prints
    std::array<int, 3> tileOf(const std::string &path)
    {
        const fs::path p = io::fromUtf8(path);
        return {std::stoi(io::toUtf8(p.parent_path().parent_path().filename())),
                std::stoi(io::toUtf8(p.parent_path().filename())), std::stoi(io::toUtf8(p.stem()))};
    }

    // The tiles runTiler lists for the zoom levels zRange of tif
    std::set<std::array<int, 3>> listedTiles(TestArea &ta, const fs::path &tif, const std::string &zRange)
    {
        std::ostringstream out;
        TilerHelper::runTiler(tif, ta.getFolder("tiles-" + zRange), out, tilerRun(OutputFormat::Text, zRange));
        std::set<std::array<int, 3>> tiles;
        std::istringstream in(out.str());
        for (std::string line; std::getline(in, line);)
            tiles.insert(tileOf(line));
        return tiles;
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
    TEST(TilerOutput, TheJsonTileListOfSeveralZoomLevelsIsAValidArray)
    {
        TestArea ta(TEST_NAME, true);
        const auto tif = makeRaster(ta);

        std::ostringstream asText, asJson;
        TilerHelper::runTiler(tif, ta.getFolder("text"), asText, tilerRun(OutputFormat::Text, "14-17"));
        TilerHelper::runTiler(tif, ta.getFolder("json"), asJson, tilerRun(OutputFormat::Json, "14-17"));

        const auto textTiles = lines(asText.str());
        ASSERT_GT(textTiles.size(), 3u);

        const json tiles = json::parse(asJson.str());
        ASSERT_TRUE(tiles.is_array());
        EXPECT_EQ(tiles.size(), textTiles.size());

        for (const auto &tile : strings(tiles))
            EXPECT_TRUE(fs::exists(io::fromUtf8(tile))) << tile;
    }

    TEST(TilerOutput, TheJsonOfASingleTileIsAOneStringArray)
    {
        TestArea ta(TEST_NAME, true);
        const auto tif = makeRaster(ta);

        std::ostringstream all;
        TilerHelper::runTiler(tif, ta.getFolder("all"), all, tilerRun(OutputFormat::Text, "16"));
        const std::string first = *lines(all.str()).begin();
        // <folder>/<z>/<x>/<y>.png, with the platform's separators
        const fs::path tile = io::fromUtf8(first);
        ASSERT_FALSE(tile.parent_path().empty()) << first;
        const std::string x = io::toUtf8(tile.parent_path().filename());
        const std::string y = io::toUtf8(tile.stem());

        std::ostringstream out;
        TilerHelper::runTiler(tif, ta.getFolder("single"), out, tilerRun(OutputFormat::Json, "16", x, y));
        const json tiles = json::parse(out.str());
        ASSERT_EQ(tiles.size(), 1u);
        EXPECT_TRUE(tiles[0].is_string());
    }

    TEST(TilerOutput, TextIsOneTilePerLine)
    {
        TestArea ta(TEST_NAME, true);
        const auto tif = makeRaster(ta);

        std::ostringstream out;
        TilerHelper::runTiler(tif, ta.getFolder("tiles"), out, tilerRun(OutputFormat::Text, "16"));
        const auto tiles = lines(out.str());
        EXPECT_GE(tiles.size(), 1u);
        for (const auto &t : tiles)
            EXPECT_TRUE(fs::exists(io::fromUtf8(t))) << t;
    }

    // A raster of one pixel of 4.9 km, the pixel of zoom level 5, has a COG of one tile of that zoom level:
    // its pixel among transparent ones. The tiles of a zoom level above it are the ones over that pixel, not
    // every tile of the tile of zoom level 5 (64 x 64 at zoom level 11, 512 x 512 at 14). The pixel is the
    // tile (3622, 5896) of zoom level 13 (rows from the south, as runTiler names them without tms)
    TEST(TilerOutput, AZoomLevelAboveARasterListsOnlyTheTilesOverItsPixels)
    {
        TestArea ta(TEST_NAME, true);
        const auto tif = writeRaster(ta, "pixel.tif",
                                     {-2318793.689736329950392, 4891.969809570309735, 0.0, 8810437.627036130055785,
                                      0.0, -4891.969809570309735},
                                     1, 1, true);

        using Tiles = std::set<std::array<int, 3>>;
        EXPECT_EQ(listedTiles(ta, tif, "11"), (Tiles{{11, 905, 1474}}));
        EXPECT_EQ(listedTiles(ta, tif, "13"), (Tiles{{13, 3622, 5896}}));
        EXPECT_EQ(listedTiles(ta, tif, "14"),
                  (Tiles{{14, 7244, 11792}, {14, 7245, 11792}, {14, 7244, 11793}, {14, 7245, 11793}}));
    }

    // Whether the tile file `path` shows a pixel: an image without an alpha band, or one whose alpha is not
    // 0 everywhere
    bool showsAPixel(const std::string &path)
    {
        auto ds = GDALDatasetUniquePtr(GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
        EXPECT_TRUE(ds != nullptr) << path;
        if (ds->GetRasterCount() != 4)
            return true;
        double minMax[2] = {0, 0};
        EXPECT_EQ(ds->GetRasterBand(4)->ComputeRasterMinMax(FALSE, minMax), CE_None);
        return minMax[1] > 0;
    }

    // The tiles runTiler lists are exactly those that show a pixel of the raster, from the zoom levels below
    // the one of its COG (15 for its 7 m pixels) to those above it: every tile of the bounds of the COG is
    // rendered, and the ones with a pixel are the tiles listed. A raster whose edges are not on the edges of
    // tiles, so the COG adds transparent pixels on every side
    TEST(TilerOutput, TheListedTilesAreExactlyTheTilesThatShowTheRaster)
    {
        TestArea ta(TEST_NAME, true);
        const auto tif =
            writeRaster(ta, "raster.tif", {1000003.0, 7.0, 0.0, 5000635.0, 0.0, -7.0}, 50, 30, true);
        RasterTiler tiler(io::toUtf8(TilerHelper::toGeoTIFF(tif)), io::toUtf8(ta.getFolder("rendered")));

        for (int z = 13; z <= 18; z++)
        {
            std::set<std::array<int, 3>> showing;
            const auto bounds = tiler.getMinMaxCoordsForZ(z);
            for (int x = bounds.min.x; x <= bounds.max.x; x++)
                for (int y = bounds.min.y; y <= bounds.max.y; y++)
                    if (showsAPixel(tiler.tile(z, x, y)))
                        showing.insert({z, x, y});
            ASSERT_FALSE(showing.empty()) << z;
            EXPECT_EQ(listedTiles(ta, tif, std::to_string(z)), showing) << "zoom level " << z;
        }
    }
}
