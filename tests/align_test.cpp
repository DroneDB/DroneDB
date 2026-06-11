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

    // ─── Helper: create a single-band Float32 GeoTIFF filled with a constant ──────
    static void createUniformGeoTiff(const std::string &path,
                                     int W, int H, double gt[6], float value)
    {
        GDALDriverH hDrv = GDALGetDriverByName("GTiff");
        GDALDatasetH hDs = GDALCreate(hDrv, path.c_str(), W, H, 1, GDT_Float32, nullptr);
        GDALSetGeoTransform(hDs, gt);

        OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(hSrs, 32632);
        char *wkt = nullptr;
        OSRExportToWkt(hSrs, &wkt);
        GDALSetProjection(hDs, wkt);
        CPLFree(wkt);
        OSRDestroySpatialReference(hSrs);

        GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
        std::vector<float> row(static_cast<size_t>(W), value);
        for (int r = 0; r < H; r++)
            GDALRasterIO(hBand, GF_Write, 0, r, W, 1,
                         row.data(), W, 1, GDT_Float32, 0, 0);
        GDALFlushCache(hDs);
        GDALClose(hDs);
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

    // ─── Test 7: VRT warp preserves a constant pixel field ───────────────────────
    // A uniform-value raster bilinearly warped must remain constant in the valid
    // interior region (bilinear interpolation of a constant C yields C exactly).
    // This validates that the VRT lightweight wrapper correctly delivers source
    // pixels to GDALWarp without losing data — the failure mode if VRT can't
    // find the source file would be an output filled with nodata or zeros.
    TEST_F(AlignTest, WarpPreservesConstantField)
    {
        TestArea ta(TEST_NAME);

        const float kValue = 137.0f;
        // Reference and source at the same position so phase correlation returns
        // zero offset (constant field has no texture to track — that's fine here;
        // the identity warp is the correct result).
        double refGt[6] = {500000.0, 2.0, 0, 5000000.0, 0, -2.0};
        double srcGt[6] = {500000.0, 2.0, 0, 5000000.0, 0, -2.0};

        auto refPath = ta.getPath("ref_const.tif").string();
        auto srcPath = ta.getPath("src_const.tif").string();
        auto outPath = ta.getPath("out_const.tif").string();

        createUniformGeoTiff(refPath, 256, 256, refGt, kValue);
        createUniformGeoTiff(srcPath, 256, 256, srcGt, kValue);

        ddb::AlignOptions opts;
        opts.mode = ddb::AlignMode::Translation;
        auto r = ddb::alignRaster(srcPath, refPath, outPath, opts);

        ASSERT_TRUE(r.success);
        ASSERT_TRUE(fs::exists(outPath));

        // Open the output COG and sample interior pixels.
        GDALDatasetH hOut = GDALOpen(outPath.c_str(), GA_ReadOnly);
        ASSERT_NE(hOut, nullptr);

        const int outW = GDALGetRasterXSize(hOut);
        const int outH = GDALGetRasterYSize(hOut);
        GDALRasterBandH hBand = GDALGetRasterBand(hOut, 1);
        int hasNd;
        const float nd = static_cast<float>(GDALGetRasterNoDataValue(hBand, &hasNd));

        // Sample a central strip well away from the nodata border introduced by
        // the COG reprojection (EPSG:3857). Bilinear of constant C = C.
        const int margin = std::max(outW, outH) / 4;
        std::vector<float> row(static_cast<size_t>(outW));
        int sampledPixels = 0, correctPixels = 0;
        for (int y = margin; y < outH - margin; y++) {
            GDALRasterIO(hBand, GF_Read, 0, y, outW, 1,
                         row.data(), outW, 1, GDT_Float32, 0, 0);
            for (int x = margin; x < outW - margin; x++) {
                if (hasNd && std::abs(row[x] - nd) < 1.0f) continue;
                sampledPixels++;
                // Tolerance 1.0 covers rounding from Float32 → bilinear → COG.
                if (std::abs(row[x] - kValue) < 1.0f) correctPixels++;
            }
        }
        GDALClose(hOut);

        EXPECT_GT(sampledPixels, 0) << "No valid interior pixels found in output";
        EXPECT_EQ(correctPixels, sampledPixels)
            << correctPixels << "/" << sampledPixels
            << " interior pixels equal " << kValue
            << " (VRT may have failed to read source pixels)";
    }

    // ─── Test 8: VRT-based warp output is deterministic ──────────────────────────
    // Running alignRaster twice with identical inputs must produce bit-identical
    // output rasters. Validates that the VRT on-demand read path is reproducible
    // (no uninitialized-buffer or partial-read artifacts from reopening the file).
    TEST_F(AlignTest, WarpOutputDeterministic)
    {
        TestArea ta(TEST_NAME);

        double refGt[6] = {500000.0, 0.5, 0, 5000000.0, 0, -0.5};
        double srcGt[6] = {500003.0, 0.5, 0, 4999998.0, 0, -0.5};  // 3 m X, -2 m Y

        auto refPath = ta.getPath("ref_det.tif").string();
        auto srcPath = ta.getPath("src_det.tif").string();
        auto out1    = ta.getPath("out_det1.tif").string();
        auto out2    = ta.getPath("out_det2.tif").string();

        createSyntheticGeoTiff(refPath, 256, 256, refGt);
        createSyntheticGeoTiff(srcPath, 256, 256, srcGt);

        ddb::AlignOptions opts;
        opts.mode = ddb::AlignMode::Translation;

        auto r1 = ddb::alignRaster(srcPath, refPath, out1, opts);
        auto r2 = ddb::alignRaster(srcPath, refPath, out2, opts);

        ASSERT_TRUE(r1.success);
        ASSERT_TRUE(r2.success);

        // Alignment results must match to sub-micron precision.
        EXPECT_NEAR(r1.txMapUnits, r2.txMapUnits, 1e-6);
        EXPECT_NEAR(r1.tyMapUnits, r2.tyMapUnits, 1e-6);

        // Output pixel data must be bit-identical.
        GDALDatasetH h1 = GDALOpen(out1.c_str(), GA_ReadOnly);
        GDALDatasetH h2 = GDALOpen(out2.c_str(), GA_ReadOnly);
        ASSERT_NE(h1, nullptr);
        ASSERT_NE(h2, nullptr);

        const int W      = GDALGetRasterXSize(h1);
        const int H      = GDALGetRasterYSize(h1);
        const int nBands = GDALGetRasterCount(h1);
        EXPECT_EQ(W, GDALGetRasterXSize(h2));
        EXPECT_EQ(H, GDALGetRasterYSize(h2));

        std::vector<float> buf1(static_cast<size_t>(W) * H);
        std::vector<float> buf2(buf1.size());
        for (int b = 1; b <= nBands; b++) {
            GDALRasterIO(GDALGetRasterBand(h1, b), GF_Read, 0, 0, W, H,
                         buf1.data(), W, H, GDT_Float32, 0, 0);
            GDALRasterIO(GDALGetRasterBand(h2, b), GF_Read, 0, 0, W, H,
                         buf2.data(), W, H, GDT_Float32, 0, 0);
            EXPECT_EQ(buf1, buf2)
                << "Band " << b << " differs between two identical alignRaster runs"
                << " (VRT read path non-determinism)";
        }
        GDALClose(h1);
        GDALClose(h2);
    }

    // ─── Helper: single-band Float32 DEM with PIXEL-coordinate elevation ────────
    // The pattern is keyed on pixel (r, c), NOT world coordinates. This is
    // required so that phase correlation can detect the XY displacement between
    // two rasters whose geotransforms differ: sampling both at the same world
    // positions in the overlap produces different pixel content (one sees rows
    // r..r+H, cols c..c+W of the pattern; the other sees a shifted window).
    // A world-coordinate function collapses after DEM standardization because
    // both srcGrid and refGrid evaluate the same f(X,Y) → identical signals →
    // dc=0, dr=0. The pixel-coordinate approach avoids this pitfall.
    static void createPixelDem(const std::string &path, int W, int H,
                               double gt[6], float zOffset)
    {
        GDALDriverH hDrv = GDALGetDriverByName("GTiff");
        GDALDatasetH hDs = GDALCreate(hDrv, path.c_str(), W, H, 1, GDT_Float32, nullptr);
        GDALSetGeoTransform(hDs, gt);

        OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(hSrs, 32632);
        char *wkt = nullptr;
        OSRExportToWkt(hSrs, &wkt);
        GDALSetProjection(hDs, wkt);
        CPLFree(wkt);
        OSRDestroySpatialReference(hSrs);

        GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
        std::vector<float> row(static_cast<size_t>(W));
        for (int r = 0; r < H; r++)
        {
            for (int c = 0; c < W; c++)
            {
                double v = 100.0 + 80.0 * hash01(r, c)
                         + 20.0 * (std::sin(r * 0.06) + std::cos(c * 0.05));
                row[c] = static_cast<float>(v) + zOffset;
            }
            GDALRasterIO(hBand, GF_Write, 0, r, W, 1, row.data(), W, 1, GDT_Float32, 0, 0);
        }
        GDALFlushCache(hDs);
        GDALClose(hDs);
    }

    // ─── Test 9: DEM vertical (Z) offset recovery on the geographic overlap ───
    // Source DEM is offset +4 m X / -3 m Y (integer pixel shift) and its
    // elevations are exactly knownZ above the reference. After alignment the
    // reported shiftZ must equal median(ref - aligned) = -knownZ, proving the
    // offset is sampled on co-located ground points (not by raw pixel index).
    TEST_F(AlignTest, DemZOffsetRecovery)
    {
        TestArea ta(TEST_NAME);
        const double knownTx = 4.0, knownTy = -3.0; // map units (m), integer px
        const float  knownZ  = 12.5f;
        double refGt[6] = {500000.0, 1.0, 0, 5000000.0, 0, -1.0};
        double srcGt[6] = {500000.0 + knownTx, 1.0, 0, 5000000.0 + knownTy, 0, -1.0};

        auto refPath = ta.getPath("ref_zdem.tif").string();
        auto srcPath = ta.getPath("src_zdem.tif").string();
        auto outPath = ta.getPath("aligned_zdem.tif").string();

        createPixelDem(refPath, 512, 512, refGt, 0.0f);
        createPixelDem(srcPath, 512, 512, srcGt, knownZ);

        ddb::AlignOptions opts;
        opts.mode = ddb::AlignMode::Translation;
        auto r = ddb::alignRaster(srcPath, refPath, outPath, opts);

        ASSERT_TRUE(r.success);
        EXPECT_TRUE(fs::exists(outPath));
        // XY alignment removes the planimetric offset.
        EXPECT_NEAR(r.txMapUnits, -knownTx, 1.5);
        EXPECT_NEAR(r.tyMapUnits, -knownTy, 1.5);
        // Vertical offset recovered on the overlap.
        EXPECT_NEAR(r.shiftZ, -static_cast<double>(knownZ), 1.0);
    }

    // ─── Test 10: translation confidence reflects the phase-correlation peak ──
    // Rich broadband texture yields a sharp peak → confidence stays in [0,1]
    // and is clearly above zero (no longer a hardcoded constant).
    TEST_F(AlignTest, TranslationConfidenceReflectsPeak)
    {
        TestArea ta(TEST_NAME);
        double refGt[6] = {500000.0, 0.5, 0, 5000000.0, 0, -0.5};
        double srcGt[6] = {500002.0, 0.5, 0, 4999999.0, 0, -0.5};

        auto refPath = ta.getPath("ref_conf.tif").string();
        auto srcPath = ta.getPath("src_conf.tif").string();
        auto outPath = ta.getPath("aligned_conf.tif").string();

        createSyntheticGeoTiff(refPath, 512, 512, refGt);
        createSyntheticGeoTiff(srcPath, 512, 512, srcGt);

        ddb::AlignOptions opts;
        opts.mode = ddb::AlignMode::Translation;
        auto r = ddb::alignRaster(srcPath, refPath, outPath, opts);

        ASSERT_TRUE(r.success);
        EXPECT_GE(r.confidence, 0.0);
        EXPECT_LE(r.confidence, 1.0);
        EXPECT_GT(r.confidence, 0.3);
    }

} // namespace
