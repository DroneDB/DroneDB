/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-13: GDAL calls whose result nobody looked at. A raster without a geotransform made the
// alignment compute with GDAL's default one (0, 1, 0, 0, 0, 1); a DEM whose rows cannot be read or
// written was "shifted" by adding the offset to whatever the failed read left in the buffer, and the
// file kept the rows it could not write.

#include <algorithm>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "align.h"
#include "exceptions.h"
#include "gdal_raii.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    constexpr int Size = 256;

    // A Float32 raster with a projection and no geotransform
    fs::path writeWithoutGeoTransform(const fs::path &path)
    {
        GDALDriverH drv = GDALGetDriverByName("GTiff");
        auto ds = adoptDataset(GDALCreate(drv, io::toUtf8(path).c_str(), 64, 64, 1, GDT_Float32, nullptr));
        EXPECT_TRUE(ds);
        const SrsPtr srs = srsFromEpsg(3857);
        char *wkt = nullptr;
        OSRExportToWkt(srs.get(), &wkt);
        GDALSetProjection(datasetHandle(ds), wkt);
        CPLFree(wkt);
        std::vector<float> row(64, 1.0f);
        for (int y = 0; y < 64; y++)
            EXPECT_EQ(GDALRasterIO(GDALGetRasterBand(datasetHandle(ds), 1), GF_Write, 0, y, 64, 1, row.data(), 64, 1,
                                   GDT_Float32, 0, 0),
                      CE_None);
        return path;
    }

    fs::path writeReference(const fs::path &path)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 64;
        spec.bands = 1;
        spec.type = GDT_Float32;
        spec.epsg = 3857;
        spec.value = [](int, int x, int y) { return 1.0 + x + y; };
        return testutils::createGeoRaster(path, spec);
    }

    // A DEFLATE-compressed Float32 raster of noise: its blocks are real compressed bytes
    fs::path writeCompressedDem(const fs::path &path, float nodata = -9999.0f)
    {
        GDALDriverH drv = GDALGetDriverByName("GTiff");
        CPLStringList options;
        options.SetNameValue("COMPRESS", "DEFLATE");
        options.SetNameValue("TILED", "YES");
        options.SetNameValue("BLOCKXSIZE", "128");
        options.SetNameValue("BLOCKYSIZE", "128");
        auto ds = adoptDataset(GDALCreate(drv, io::toUtf8(path).c_str(), Size, Size, 1, GDT_Float32, options.List()));
        EXPECT_TRUE(ds);
        GDALRasterBandH band = GDALGetRasterBand(datasetHandle(ds), 1);
        GDALSetRasterNoDataValue(band, nodata);
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> noise(0.0f, 100.0f);
        std::vector<float> row(Size);
        for (int y = 0; y < Size; y++)
        {
            for (auto &v : row)
                v = noise(rng);
            if (y == 5)
                row[3] = nodata;
            EXPECT_EQ(GDALRasterIO(band, GF_Write, 0, y, Size, 1, row.data(), Size, 1, GDT_Float32, 0, 0), CE_None);
        }
        return path;
    }

    std::vector<float> readRows(const fs::path &path)
    {
        auto ds = adoptDataset(GDALOpen(io::toUtf8(path).c_str(), GA_ReadOnly));
        std::vector<float> data(static_cast<size_t>(Size) * Size);
        EXPECT_EQ(GDALRasterIO(GDALGetRasterBand(datasetHandle(ds), 1), GF_Read, 0, 0, Size, Size, data.data(), Size,
                               Size, GDT_Float32, 0, 0),
                  CE_None);
        return data;
    }

    // Overwrites a stretch of the file between its header and the end of the first tile data
    void corruptPixelData(const fs::path &path)
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(64);
        const std::string junk(static_cast<size_t>(Size) * 40, '\x5A');
        f.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }

    // Whether GDAL can read the whole band
    bool bandIsReadable(const fs::path &path)
    {
        auto ds = adoptDataset(GDALOpen(io::toUtf8(path).c_str(), GA_ReadOnly));
        if (!ds)
            return false;
        std::vector<float> data(static_cast<size_t>(Size) * Size);
        CPLPushErrorHandler(CPLQuietErrorHandler);
        const CPLErr err = GDALRasterIO(GDALGetRasterBand(datasetHandle(ds), 1), GF_Read, 0, 0, Size, Size, data.data(),
                                        Size, Size, GDT_Float32, 0, 0);
        CPLPopErrorHandler();
        return err == CE_None;
    }

    TEST(Dq13AlignChecks, ValidationRejectsARasterWithoutAGeoTransform)
    {
        TestArea ta(TEST_NAME, true);
        const auto bare = writeWithoutGeoTransform(ta.getPath("bare.tif"));
        const auto reference = writeReference(ta.getPath("reference.tif"));

        const auto asSource = validateAlignRaster(io::toUtf8(bare), io::toUtf8(reference));
        EXPECT_FALSE(asSource.ok);
        ASSERT_FALSE(asSource.errors.empty());
        EXPECT_NE(asSource.errors.back().find("geotransform"), std::string::npos) << asSource.errors.back();

        const auto asReference = validateAlignRaster(io::toUtf8(reference), io::toUtf8(bare));
        EXPECT_FALSE(asReference.ok);
        ASSERT_FALSE(asReference.errors.empty());
        EXPECT_NE(asReference.errors.back().find("geotransform"), std::string::npos) << asReference.errors.back();

        EXPECT_THROW(alignRaster(io::toUtf8(bare), io::toUtf8(reference), io::toUtf8(ta.getPath("out.tif"))), AppException);
        EXPECT_FALSE(fs::exists(ta.getPath("out.tif")));
    }

    TEST(Dq13AlignChecks, AGeoreferencedPairStillValidates)
    {
        TestArea ta(TEST_NAME, true);
        const auto a = writeReference(ta.getPath("a.tif"));
        const auto b = writeReference(ta.getPath("b.tif"));
        const auto result = validateAlignRaster(io::toUtf8(a), io::toUtf8(b));
        EXPECT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors[0]);
    }

    TEST(Dq13ZOffset, ShiftsEveryValueExceptNodata)
    {
        TestArea ta(TEST_NAME, true);
        const auto dem = writeCompressedDem(ta.getPath("dem.tif"));
        const auto before = readRows(dem);

        applyZOffset(io::toUtf8(dem), 2.5);

        const auto after = readRows(dem);
        ASSERT_EQ(before.size(), after.size());
        for (size_t i = 0; i < before.size(); i++)
        {
            if (before[i] == -9999.0f)
                EXPECT_EQ(after[i], -9999.0f) << i;
            else
                ASSERT_FLOAT_EQ(after[i], before[i] + 2.5f) << i;
        }
    }

    TEST(Dq13ZOffset, ATinyOffsetLeavesTheFileAlone)
    {
        TestArea ta(TEST_NAME, true);
        const auto dem = writeCompressedDem(ta.getPath("dem.tif"));
        const auto before = readRows(dem);
        applyZOffset(io::toUtf8(dem), 1e-9);
        EXPECT_EQ(readRows(dem), before);
    }

    TEST(Dq13ZOffset, ARasterWhoseRowsCannotBeReadIsAnErrorNotAShiftedBuffer)
    {
        TestArea ta(TEST_NAME, true);
        const auto dem = writeCompressedDem(ta.getPath("dem.tif"));
        corruptPixelData(dem);
        ASSERT_FALSE(bandIsReadable(dem)) << "the corruption must break the decoding for this test to mean anything";

        EXPECT_THROW(applyZOffset(io::toUtf8(dem), 2.5), GDALException);
    }

    TEST(Dq13ZOffset, AMissingFileIsAnError)
    {
        TestArea ta(TEST_NAME, true);
        EXPECT_THROW(applyZOffset(io::toUtf8(ta.getPath("missing.tif")), 2.5), GDALException);
    }
}
