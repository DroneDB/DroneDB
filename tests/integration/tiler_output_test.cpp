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
#include "las_test_writer.h"
#include "mio.h"
#include "pointcloud.h"
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

    // Whether the tile file `path` shows a pixel: an image without an alpha band, or one whose alpha is not
    // 0 everywhere
    bool showsAPixel(const std::string &path)
    {
        auto ds = GDALDatasetUniquePtr(GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
        if (!ds)
        {
            ADD_FAILURE() << "no tile " << path;
            return false;
        }
        if (ds->GetRasterCount() != 4)
            return true;
        double minMax[2] = {0, 0};
        EXPECT_EQ(ds->GetRasterBand(4)->ComputeRasterMinMax(FALSE, minMax), CE_None);
        return minMax[1] > 0;
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

    // The tiles runTiler lists for the zoom levels zRange of input, with XYZ rows (TMS rows with `tms`)
    std::set<std::array<int, 3>> listedTiles(TestArea &ta, const fs::path &input, const std::string &zRange,
                                             bool tms = false)
    {
        std::ostringstream out;
        TilerRun run = tilerRun(OutputFormat::Text, zRange);
        run.tms = tms;
        TilerHelper::runTiler(input, ta.getFolder(std::string(tms ? "tms-" : "xyz-") + zRange), out, run);
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

    // A raster of one pixel of 4.9 km, the pixel of zoom level 5: the tile (3622, 2295) of zoom level 13 (XYZ
    // rows, from the north; 5896 from the south)
    fs::path writeOnePixelRaster(TestArea &ta)
    {
        return writeRaster(ta, "pixel.tif",
                           {-2318793.689736329950392, 4891.969809570309735, 0.0, 8810437.627036130055785, 0.0,
                            -4891.969809570309735},
                           1, 1, true);
    }

    // The raster of one pixel has a COG of one tile of zoom level 5: its pixel among transparent ones. The
    // tiles of a zoom level above it are the ones over that pixel, not every tile of the tile of zoom level 5
    // (64 x 64 at zoom level 11, 512 x 512 at 14)
    TEST(TilerOutput, AZoomLevelAboveARasterListsOnlyTheTilesOverItsPixels)
    {
        TestArea ta(TEST_NAME, true);
        const auto tif = writeOnePixelRaster(ta);

        using Tiles = std::set<std::array<int, 3>>;
        EXPECT_EQ(listedTiles(ta, tif, "11"), (Tiles{{11, 905, 573}}));
        EXPECT_EQ(listedTiles(ta, tif, "13"), (Tiles{{13, 3622, 2295}}));
        EXPECT_EQ(listedTiles(ta, tif, "14"),
                  (Tiles{{14, 7244, 4591}, {14, 7245, 4591}, {14, 7244, 4590}, {14, 7245, 4590}}));
    }

    // The row of a tile of zoom level z counted from the other side (XYZ from TMS and back)
    int flippedRow(int z, int y)
    {
        return (1 << z) - 1 - y;
    }

    std::set<std::array<int, 3>> withFlippedRows(const std::set<std::array<int, 3>> &tiles)
    {
        std::set<std::array<int, 3>> result;
        for (const auto &[z, x, y] : tiles)
            result.insert({z, x, flippedRow(z, y)});
        return result;
    }

    // `ddb tile` writes XYZ rows, as its help says, and TMS rows with --tms: it wrote TMS rows without
    // --tms and XYZ rows with it (the flag of the tilers, inverted with respect to its name, was passed as
    // the option)
    TEST(TilerOutput, TheTilesOfARasterHaveXyzRowsUnlessTmsIsAsked)
    {
        TestArea ta(TEST_NAME, true);
        const auto tif = writeOnePixelRaster(ta);

        using Tiles = std::set<std::array<int, 3>>;
        EXPECT_EQ(listedTiles(ta, tif, "13"), (Tiles{{13, 3622, 2295}}));
        EXPECT_EQ(listedTiles(ta, tif, "13", true), (Tiles{{13, 3622, 5896}}));
        EXPECT_TRUE(showsAPixel(io::toUtf8(ta.getFolder("xyz-13") / "13" / "3622" / "2295.png")));
        EXPECT_TRUE(showsAPixel(io::toUtf8(ta.getFolder("tms-13") / "13" / "3622" / "5896.png")));
    }

    // -x and -y name an XYZ tile, a TMS tile with --tms: the tile of the pixel, written where its rows say
    TEST(TilerOutput, ASingleTileIsAnXyzTileUnlessTmsIsAsked)
    {
        TestArea ta(TEST_NAME, true);
        const auto tif = writeOnePixelRaster(ta);

        const auto single = [&](const std::string &folder, int y, bool tms)
        {
            std::ostringstream out;
            TilerRun run = tilerRun(OutputFormat::Text, "13", "3622", std::to_string(y));
            run.tms = tms;
            TilerHelper::runTiler(tif, ta.getFolder(folder), out, run);
            return lines(out.str());
        };
        const auto xyz = single("xyz", 2295, false);
        ASSERT_EQ(xyz.size(), 1u);
        EXPECT_EQ(tileOf(*xyz.begin()), (std::array<int, 3>{13, 3622, 2295}));
        EXPECT_TRUE(showsAPixel(*xyz.begin()));

        const auto tms = single("tms", 5896, true);
        ASSERT_EQ(tms.size(), 1u);
        EXPECT_EQ(tileOf(*tms.begin()), (std::array<int, 3>{13, 3622, 5896}));
        EXPECT_TRUE(showsAPixel(*tms.begin()));
    }

    // The tiles of a point cloud have XYZ rows too: a cloud of a few metres in EPSG:3857 at (1000000,
    // 5000000) is in the tile (17201, 12295) of zoom level 15 (row 20472 from the south)
    TEST(TilerOutput, TheTilesOfAPointCloudHaveXyzRowsUnlessTmsIsAsked)
    {
        TestArea ta(TEST_NAME, true);
        const auto las = ta.getPath("cloud.las");
        OGRSpatialReference srs;
        srs.importFromEPSG(3857);
        char *wkt = nullptr;
        srs.exportToWkt(&wkt);
        const std::string crs = wkt;
        CPLFree(wkt);
        testutils::writeLas14(las, crs,
                              {{1000000.0, 5000000.0, 10.0}, {1000004.0, 5000003.0, 11.0},
                               {1000008.0, 5000001.0, 12.0}, {1000002.0, 5000006.0, 13.0}});
        const fs::path copcFolder = ta.getFolder("copc");
        buildCopc({las.string()}, copcFolder.string());
        const fs::path copc = copcFolder / "cloud.copc.laz";

        const auto xyz = listedTiles(ta, copc, "15");
        EXPECT_EQ(xyz, (std::set<std::array<int, 3>>{{15, 17201, 12295}}));
        EXPECT_EQ(listedTiles(ta, copc, "15", true), withFlippedRows(xyz));
        EXPECT_TRUE(fs::exists(ta.getFolder("xyz-15") / "15" / "17201" / "12295.png"));
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
        const auto tiler = TilerHelper::rasterTilerFor(tif, io::toUtf8(ta.getFolder("rendered")));

        for (int z = 13; z <= 18; z++)
        {
            std::set<std::array<int, 3>> showing;
            const auto bounds = tiler->getMinMaxCoordsForZ(z);
            for (int x = bounds.min.x; x <= bounds.max.x; x++)
                for (int y = bounds.min.y; y <= bounds.max.y; y++)
                    if (showsAPixel(tiler->tile(z, x, y)))
                        showing.insert({z, x, flippedRow(z, y)}); // the tiler takes TMS rows
            ASSERT_FALSE(showing.empty()) << z;
            EXPECT_EQ(listedTiles(ta, tif, std::to_string(z)), showing) << "zoom level " << z;
        }
    }
}
