/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "cog.h"
#include "mask.h"
#include "ddb.h"
#include "exceptions.h"
#include "gdal_inc.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using namespace ddb;

// ---------------------------------------------------------------------------
// Helper: create a small synthetic RGB GeoTIFF with a black border collar.
// The centre pixels are set to (128, 64, 32); the outer border ring is black
// (0, 0, 0). EPSG:32632 (UTM Zone 32N) is used so buildCog can reproject.
// ---------------------------------------------------------------------------
static fs::path createRgbWithBlackBorder(const fs::path &path,
                                         int w = 64, int h = 64,
                                         int borderPx = 8)
{
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    if (!drv) throw std::runtime_error("GTiff driver not available");

    GDALDatasetH hDs = GDALCreate(drv, path.string().c_str(),
                                   w, h, 3, GDT_Byte, nullptr);
    if (!hDs) throw std::runtime_error("Cannot create test raster");

    // Minimal georeference in EPSG:32632 so the COG builder can reproject.
    double gt[6] = {500000.0, 1.0, 0.0, 4500000.0, 0.0, -1.0};
    GDALSetGeoTransform(hDs, gt);

    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 32632);
    char *wkt = nullptr;
    OSRExportToWkt(srs, &wkt);
    GDALSetProjection(hDs, wkt);
    CPLFree(wkt);
    OSRDestroySpatialReference(srs);

    // Fill each band: border pixels = 0 (black collar), interior = colour value.
    const uint8_t colours[3] = {128, 64, 32};
    std::vector<uint8_t> row(static_cast<size_t>(w));

    for (int b = 1; b <= 3; b++) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDs, b);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                bool isBorder = (x < borderPx || x >= w - borderPx ||
                                 y < borderPx || y >= h - borderPx);
                row[static_cast<size_t>(x)] = isBorder ? 0 : colours[b - 1];
            }
            GDALRasterIO(hBand, GF_Write, 0, y, w, 1,
                         row.data(), w, 1, GDT_Byte, 0, 0);
        }
    }

    GDALFlushCache(hDs);
    GDALClose(hDs);
    return path;
}

// ---------------------------------------------------------------------------
// Helper: create a 4-band Byte GeoTIFF where band 4 has NO alpha colour
// interpretation (simulating an RGB+NIR multispectral raster).
// ---------------------------------------------------------------------------
static fs::path createFourBandNonAlpha(const fs::path &path)
{
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    if (!drv) throw std::runtime_error("GTiff driver not available");

    GDALDatasetH hDs = GDALCreate(drv, path.string().c_str(),
                                   32, 32, 4, GDT_Byte, nullptr);
    if (!hDs) throw std::runtime_error("Cannot create test raster");

    // Bands 1-4: set to GCI_GrayIndex (or leave default = undefined).
    // Deliberately do NOT set band 4 to GCI_AlphaBand.
    for (int b = 1; b <= 4; b++) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDs, b);
        GDALSetRasterColorInterpretation(hBand, GCI_GrayIndex);
    }

    double gt[6] = {500000.0, 1.0, 0.0, 4500000.0, 0.0, -1.0};
    GDALSetGeoTransform(hDs, gt);

    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 32632);
    char *wkt = nullptr;
    OSRExportToWkt(srs, &wkt);
    GDALSetProjection(hDs, wkt);
    CPLFree(wkt);
    OSRDestroySpatialReference(srs);

    GDALFlushCache(hDs);
    GDALClose(hDs);
    return path;
}

// ===========================================================================
// maskBorders tests
// ===========================================================================

TEST(mask, rejectsFourBandNonAlpha) {
    TestArea ta(TEST_NAME);
    fs::path input = createFourBandNonAlpha(ta.getPath("4band_nir.tif"));
    fs::path output = ta.getPath("4band_nir_masked.tif");

    EXPECT_THROW(
        ddb::maskBorders(input.string(), output.string()),
        InvalidArgsException
    ) << "4-band input without an alpha band should be rejected";
}

TEST(mask, acceptsFourBandWithAlpha) {
    TestArea ta(TEST_NAME);

    // Build a 4-band RGBA raster (band 4 = GCI_AlphaBand).
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    ASSERT_NE(drv, nullptr);
    fs::path input = ta.getPath("rgba_input.tif");
    GDALDatasetH hDs = GDALCreate(drv, input.string().c_str(),
                                   64, 64, 4, GDT_Byte, nullptr);
    ASSERT_NE(hDs, nullptr);

    double gt[6] = {500000.0, 1.0, 0.0, 4500000.0, 0.0, -1.0};
    GDALSetGeoTransform(hDs, gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 32632);
    char *wkt = nullptr;
    OSRExportToWkt(srs, &wkt);
    GDALSetProjection(hDs, wkt);
    CPLFree(wkt);
    OSRDestroySpatialReference(srs);

    // Mark band 4 as alpha and fill with full-opacity (255).
    GDALRasterBandH hBand4 = GDALGetRasterBand(hDs, 4);
    GDALSetRasterColorInterpretation(hBand4, GCI_AlphaBand);
    std::vector<uint8_t> row(64, 255);
    for (int y = 0; y < 64; y++)
        GDALRasterIO(hBand4, GF_Write, 0, y, 64, 1, row.data(), 64, 1, GDT_Byte, 0, 0);

    GDALFlushCache(hDs);
    GDALClose(hDs);

    fs::path output = ta.getPath("rgba_masked.tif");
    EXPECT_NO_THROW(ddb::maskBorders(input.string(), output.string()))
        << "4-band RGBA input should be accepted by maskBorders";
    EXPECT_TRUE(fs::exists(output));
}

// ===========================================================================
// COG regression: PER_DATASET mask must be preserved after buildCog
// ===========================================================================

TEST(mask, cogPreservesPerDatasetMask) {
    TestArea ta(TEST_NAME);

    // 1. Create a small RGB raster with a black border collar.
    fs::path rgbInput = createRgbWithBlackBorder(ta.getPath("rgb_input.tif"));

    // 2. Run maskBorders -> produces an internal PER_DATASET mask (dataset mask,
    //    not an alpha band) via nearblack -setmask.
    fs::path maskedGtiff = ta.getPath("rgb_masked.tif");
    ASSERT_NO_THROW(ddb::maskBorders(rgbInput.string(), maskedGtiff.string()));
    ASSERT_TRUE(fs::exists(maskedGtiff));

    // Sanity: the intermediate GeoTIFF must carry a stand-alone PER_DATASET mask.
    {
        GDALDatasetH hDs = GDALOpen(maskedGtiff.string().c_str(), GA_ReadOnly);
        ASSERT_NE(hDs, nullptr);
        GDALRasterBandH hBand1 = GDALGetRasterBand(hDs, 1);
        ASSERT_NE(hBand1, nullptr);
        const int maskFlags = GDALGetMaskFlags(hBand1);
        GDALClose(hDs);

        const bool hasPerDatasetMask =
            (maskFlags & GMF_PER_DATASET) != 0 &&
            (maskFlags & GMF_ALPHA) == 0 &&
            (maskFlags & GMF_NODATA) == 0;
        EXPECT_TRUE(hasPerDatasetMask)
            << "maskBorders output should carry a PER_DATASET (non-alpha) mask; "
               "got mask flags = " << maskFlags;
    }

    // 3. Build COG from the masked intermediate.
    fs::path cogOutput = ta.getPath("rgb_cog.tif");
    ASSERT_NO_THROW(ddb::buildCog(maskedGtiff.string(), cogOutput.string()));
    ASSERT_TRUE(fs::exists(cogOutput));

    // 4. The COG must expose transparency (alpha band or PER_DATASET mask).
    //    buildCog converts the PER_DATASET mask to a -dstalpha during the warp
    //    step, so the COG output carries an alpha band (GMF_ALPHA | GMF_PER_DATASET).
    {
        GDALDatasetH hCog = GDALOpen(cogOutput.string().c_str(), GA_ReadOnly);
        ASSERT_NE(hCog, nullptr);

        GDALRasterBandH hBand1 = GDALGetRasterBand(hCog, 1);
        ASSERT_NE(hBand1, nullptr);
        const int maskFlags = GDALGetMaskFlags(hBand1);
        const bool hasTransparency =
            (maskFlags & GMF_ALPHA) != 0 ||
            ((maskFlags & GMF_PER_DATASET) != 0 &&
             (maskFlags & GMF_NODATA) == 0 &&
             GDALGetRasterCount(hCog) > 3);
        GDALClose(hCog);

        EXPECT_TRUE(hasTransparency)
            << "COG built from a PER_DATASET-masked raster must preserve "
               "transparency; got mask flags = " << maskFlags;
    }
}

} // namespace
