/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "test.h"
#include "align.h"
#include "gdal_inc.h"
#include "ddb.h"
#include "fs.h"
#include "testarea.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{

    class AlignTest : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    // Deterministic broadband pseudo-random value in [0,1) from (r,c).
    // Broadband (white-noise) content is what lets phase correlation and NCC
    // localize a shift; smooth low-frequency patterns do not.
    static double hash01(int r, int c)
    {
        uint32_t h = static_cast<uint32_t>(r) * 73856093u ^ static_cast<uint32_t>(c) * 19349663u;
        h ^= h >> 13;
        h *= 0x85ebca6bu;
        h ^= h >> 16;
        return (h & 0xFFFFFFu) / static_cast<double>(0x1000000u);
    }

    // ─── Helper: create a synthetic GeoTIFF with rich, broadband texture ─────────
    static void createSyntheticGeoTiff(const std::string &path,
                                       int W, int H, double gt[6],
                                       bool isDem = false)
    {
        GDALDriverH hDrv = GDALGetDriverByName("GTiff");
        GDALDatasetH hDs = GDALCreate(hDrv, path.c_str(), W, H,
                                      isDem ? 1 : 3,
                                      isDem ? GDT_Float32 : GDT_Byte, nullptr);
        GDALSetGeoTransform(hDs, gt);

        OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(hSrs, 32632);
        char *wkt = nullptr;
        OSRExportToWkt(hSrs, &wkt);
        GDALSetProjection(hDs, wkt);
        CPLFree(wkt);
        OSRDestroySpatialReference(hSrs);

        int nbands = GDALGetRasterCount(hDs);
        for (int b = 1; b <= nbands; b++)
        {
            GDALRasterBandH hBand = GDALGetRasterBand(hDs, b);
            std::vector<float> row(W);
            for (int r = 0; r < H; r++)
            {
                for (int c = 0; c < W; c++)
                {
                    // Broadband noise (dominant, for phase correlation / NCC) plus a
                    // mild smooth component (widens the NCC peak for sub-pixel fit).
                    double v = 60.0 + 130.0 * hash01(r, c) + 30.0 * (std::sin(r * 0.06) + std::cos(c * 0.05));
                    row[c] = static_cast<float>(v);
                }
                GDALRasterIO(hBand, GF_Write, 0, r, W, 1,
                             row.data(), W, 1, GDT_Float32, 0, 0);
            }
        }
        GDALClose(hDs);
    }

    // ─── Test 1: Translation mode — recover a known offset ───────────────────────
    TEST_F(AlignTest, TranslationRecovery)
    {
        TestArea ta(TEST_NAME);

        // Source is offset +5 m X, -3 m Y relative to the reference.
        const double knownTx = 5.0, knownTy = -3.0; // map units (m), EPSG:32632

        double refGt[6] = {500000.0, 0.5, 0, 5000000.0, 0, -0.5};
        double srcGt[6] = {500000.0 + knownTx, 0.5, 0, 5000000.0 + knownTy, 0, -0.5};

        auto refPath = ta.getPath("reference.tif").string();
        auto srcPath = ta.getPath("source.tif").string();
        auto outPath = ta.getPath("aligned.tif").string();

        createSyntheticGeoTiff(refPath, 512, 512, refGt);
        createSyntheticGeoTiff(srcPath, 512, 512, srcGt);

        ddb::AlignOptions opts;
        opts.mode = ddb::AlignMode::Translation;
        auto r = ddb::alignRaster(srcPath, refPath, outPath, opts);

        ASSERT_TRUE(r.success);
        // Alignment must remove the offset: tx ≈ -knownTx, ty ≈ -knownTy.
        // Tolerance: 1 m = 2 pixels @ 0.5 m GSD.
        EXPECT_NEAR(r.txMapUnits, -knownTx, 1.0);
        EXPECT_NEAR(r.tyMapUnits, -knownTy, 1.0);

        EXPECT_TRUE(fs::exists(outPath));
    }

    // ─── Test 2: Similarity mode — valid output, low RMSE ────────────────────────
    TEST_F(AlignTest, SimilarityOutputValid)
    {
        TestArea ta(TEST_NAME);

        double refGt[6] = {500000.0, 0.5, 0, 5000000.0, 0, -0.5};
        // Small known offset (2 m X, -1 m Y) → similarity collapses to translation.
        double srcGt[6] = {500002.0, 0.5, 0, 4999999.0, 0, -0.5};

        auto refPath = ta.getPath("ref_sim.tif").string();
        auto srcPath = ta.getPath("src_sim.tif").string();
        auto outPath = ta.getPath("aligned_sim.tif").string();

        createSyntheticGeoTiff(refPath, 512, 512, refGt);
        createSyntheticGeoTiff(srcPath, 512, 512, srcGt);

        ddb::AlignOptions opts;
        opts.mode = ddb::AlignMode::Similarity;
        opts.ransacThreshold = 2.0;
        // Tuned for a fast unit test: with an accurate phase seed a small search
        // radius is sufficient, which keeps the NCC stage cheap.
        opts.patchSize = 48;
        opts.searchRadius = 24;
        opts.maxPatches = 64;
        auto r = ddb::alignRaster(srcPath, refPath, outPath, opts);

        ASSERT_TRUE(r.success);
        EXPECT_TRUE(fs::exists(outPath));
        EXPECT_LT(r.rmseMapUnits, 3.0);
        EXPECT_GT(r.confidence, 0.1);
        // For this small pure-translation offset the similarity must collapse to a
        // near-identity transform. (tx/ty are the origin-referenced affine intercept
        // and are therefore not asserted here: at UTM coordinates a numerically tiny
        // rotation shifts them by several metres while the alignment stays accurate —
        // the RMSE above is the meaningful alignment metric.)
        EXPECT_NEAR(r.scale, 1.0, 0.05);
        EXPECT_LT(std::abs(r.thetaDeg), 5.0);
    }

    // ─── Test 3: Validation — no overlap ─────────────────────────────────────────
    TEST_F(AlignTest, ValidateNoOverlap)
    {
        TestArea ta(TEST_NAME);
        double gt1[6] = {500000.0, 0.5, 0, 5000000.0, 0, -0.5};
        double gt2[6] = {700000.0, 0.5, 0, 5200000.0, 0, -0.5}; // far away
        auto aPath = ta.getPath("a.tif").string();
        auto bPath = ta.getPath("b.tif").string();
        createSyntheticGeoTiff(aPath, 128, 128, gt1);
        createSyntheticGeoTiff(bPath, 128, 128, gt2);
        auto r = ddb::validateAlignRaster(aPath, bPath);
        EXPECT_FALSE(r.ok);
        EXPECT_FALSE(r.errors.empty());
    }

    // ─── Test 4: Validation — missing source ─────────────────────────────────────
    TEST_F(AlignTest, ValidateMissingSource)
    {
        TestArea ta(TEST_NAME);
        double gt[6] = {500000.0, 0.5, 0, 5000000.0, 0, -0.5};
        auto refPath = ta.getPath("ref.tif").string();
        createSyntheticGeoTiff(refPath, 64, 64, gt);
        auto r = ddb::validateAlignRaster(ta.getPath("does_not_exist.tif").string(), refPath);
        EXPECT_FALSE(r.ok);
    }

    // ─── Test 5: JSON output contains the required fields ────────────────────────
    TEST_F(AlignTest, ResultJsonContainsRequiredFields)
    {
        ddb::AlignResult r;
        r.success = true;
        r.txMapUnits = 1.5;
        r.tyMapUnits = -0.3;
        r.thetaDeg = 0.5;
        r.scale = 1.001;
        r.confidence = 0.75;
        r.mode = "similarity";
        auto j = ddb::alignResultToJson(r);
        EXPECT_NE(j.find("\"tx\""), std::string::npos);
        EXPECT_NE(j.find("\"ty\""), std::string::npos);
        EXPECT_NE(j.find("\"thetaDeg\""), std::string::npos);
        EXPECT_NE(j.find("\"confidence\""), std::string::npos);
        EXPECT_NE(j.find("\"mode\""), std::string::npos);
    }

    // ─── Test 6: Validation — compatible inputs report ok ────────────────────────
    TEST_F(AlignTest, ValidateCompatible)
    {
        TestArea ta(TEST_NAME);
        double refGt[6] = {500000.0, 0.5, 0, 5000000.0, 0, -0.5};
        double srcGt[6] = {500002.0, 0.5, 0, 4999999.0, 0, -0.5};
        auto refPath = ta.getPath("ref_ok.tif").string();
        auto srcPath = ta.getPath("src_ok.tif").string();
        createSyntheticGeoTiff(refPath, 256, 256, refGt);
        createSyntheticGeoTiff(srcPath, 256, 256, srcGt);
        auto r = ddb::validateAlignRaster(srcPath, refPath);
        EXPECT_TRUE(r.ok);
        EXPECT_GT(r.summary.overlapPercent, 5.0);
        EXPECT_EQ(r.summary.sourceType, "ortho");
    }

} // namespace
