/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "test.h"
#include "ddb.h"
#include "gdal_inc.h"
#include "fs.h"
#include "testarea.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
    class ExportRasterTest : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    // Deterministic broadband pseudo-random value in [0,1) from (r,c).
    static double hash01(int r, int c)
    {
        uint32_t h = static_cast<uint32_t>(r) * 73856093u ^ static_cast<uint32_t>(c) * 19349663u;
        h ^= h >> 13;
        h *= 0x85ebca6bu;
        h ^= h >> 16;
        return (h & 0xFFFFFFu) / static_cast<double>(0x1000000u);
    }

    // ─── Helper: 3-band Byte ortho GeoTIFF with broadband texture (EPSG:32632) ──
    static void createOrtho3Band(const std::string &path, int W, int H, double gt[6])
    {
        GDALDriverH hDrv = GDALGetDriverByName("GTiff");
        GDALDatasetH hDs = GDALCreate(hDrv, path.c_str(), W, H, 3, GDT_Byte, nullptr);
        GDALSetGeoTransform(hDs, gt);

        OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(hSrs, 32632);
        char *wkt = nullptr;
        OSRExportToWkt(hSrs, &wkt);
        GDALSetProjection(hDs, wkt);
        CPLFree(wkt);
        OSRDestroySpatialReference(hSrs);

        std::vector<uint8_t> row(static_cast<size_t>(W));
        for (int b = 1; b <= 3; b++)
        {
            GDALRasterBandH hBand = GDALGetRasterBand(hDs, b);
            for (int r = 0; r < H; r++)
            {
                for (int c = 0; c < W; c++)
                {
                    double v = 40.0 + 200.0 * hash01(r, c + b * 7);
                    row[c] = static_cast<uint8_t>(std::min(255.0, std::max(0.0, v)));
                }
                GDALRasterIO(hBand, GF_Write, 0, r, W, 1, row.data(), W, 1, GDT_Byte, 0, 0);
            }
        }
        GDALFlushCache(hDs);
        GDALClose(hDs);
    }

    // ─── Test 1: formula export → tiled, overviewed, georeferenced RGBA ─────────
    TEST_F(ExportRasterTest, FormulaExportProducesTiledOverviewedRgba)
    {
        TestArea ta(TEST_NAME);
        double gt[6] = {500000.0, 0.5, 0, 5000000.0, 0, -0.5};
        auto inPath  = ta.getPath("in_ortho.tif").string();
        auto outPath = ta.getPath("out_vari.tif").string();
        createOrtho3Band(inPath, 1024, 1024, gt);

        // VARI uses R/G/B only; pass an explicit band filter so detection is
        // unambiguous for a generic (sensor-less) raster.
        DDBErr err = DDBExportRaster(inPath.c_str(), outPath.c_str(),
                                     nullptr, nullptr, "VARI", "RGB",
                                     "rdylgn", nullptr);
        ASSERT_EQ(err, DDBERR_NONE);
        ASSERT_TRUE(fs::exists(outPath));

        GDALDatasetH hOut = GDALOpen(outPath.c_str(), GA_ReadOnly);
        ASSERT_NE(hOut, nullptr);

        // Formula mode always writes 4 bands (RGBA).
        EXPECT_EQ(GDALGetRasterCount(hOut), 4);

        // Tiled (not stripped): a stripped GeoTIFF has block height 1.
        int bx = 0, by = 0;
        GDALGetBlockSize(GDALGetRasterBand(hOut, 1), &bx, &by);
        EXPECT_GT(by, 1) << "Output should be tiled (block height " << by << ")";

        // Internal overviews present for efficient zoomed reads.
        EXPECT_GT(GDALGetOverviewCount(GDALGetRasterBand(hOut, 1)), 0);

        // Georeferencing preserved from the source.
        double gtOut[6];
        ASSERT_EQ(GDALGetGeoTransform(hOut, gtOut), CE_None);
        EXPECT_NEAR(gtOut[0], gt[0], 1e-6);
        EXPECT_NEAR(gtOut[1], gt[1], 1e-6);
        EXPECT_NEAR(gtOut[3], gt[3], 1e-6);
        EXPECT_NEAR(gtOut[5], gt[5], 1e-6);

        // 4th band must be flagged as alpha.
        EXPECT_EQ(GDALGetRasterColorInterpretation(GDALGetRasterBand(hOut, 4)), GCI_AlphaBand);

        GDALClose(hOut);
    }

    // ─── Test 2: band-selection export → tiled + overviews, bands preserved ─────
    TEST_F(ExportRasterTest, BandSelectionExportProducesTiledOverviews)
    {
        TestArea ta(TEST_NAME);
        double gt[6] = {500000.0, 0.5, 0, 5000000.0, 0, -0.5};
        auto inPath  = ta.getPath("in_bands.tif").string();
        auto outPath = ta.getPath("out_bands.tif").string();
        createOrtho3Band(inPath, 1024, 1024, gt);

        DDBErr err = DDBExportRaster(inPath.c_str(), outPath.c_str(),
                                     nullptr, "1,2,3", nullptr, nullptr,
                                     nullptr, nullptr);
        ASSERT_EQ(err, DDBERR_NONE);
        ASSERT_TRUE(fs::exists(outPath));

        GDALDatasetH hOut = GDALOpen(outPath.c_str(), GA_ReadOnly);
        ASSERT_NE(hOut, nullptr);

        EXPECT_EQ(GDALGetRasterCount(hOut), 3);

        int bx = 0, by = 0;
        GDALGetBlockSize(GDALGetRasterBand(hOut, 1), &bx, &by);
        EXPECT_GT(by, 1) << "Output should be tiled (block height " << by << ")";

        EXPECT_GT(GDALGetOverviewCount(GDALGetRasterBand(hOut, 1)), 0);

        double gtOut[6];
        ASSERT_EQ(GDALGetGeoTransform(hOut, gtOut), CE_None);
        EXPECT_NEAR(gtOut[0], gt[0], 1e-6);
        EXPECT_NEAR(gtOut[1], gt[1], 1e-6);

        GDALClose(hOut);
    }

    // ─── Test 3: invalid formula is rejected ────────────────────────────────────
    TEST_F(ExportRasterTest, UnknownFormulaFails)
    {
        TestArea ta(TEST_NAME);
        double gt[6] = {500000.0, 0.5, 0, 5000000.0, 0, -0.5};
        auto inPath  = ta.getPath("in_bad.tif").string();
        auto outPath = ta.getPath("out_bad.tif").string();
        createOrtho3Band(inPath, 256, 256, gt);

        DDBErr err = DDBExportRaster(inPath.c_str(), outPath.c_str(),
                                     nullptr, nullptr, "NOT_A_FORMULA", "RGB",
                                     "rdylgn", nullptr);
        EXPECT_NE(err, DDBERR_NONE);
    }

} // namespace
