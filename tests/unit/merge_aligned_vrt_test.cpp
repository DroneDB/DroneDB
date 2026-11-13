/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The merge of bands that are not aligned stacks a window of each input (merge_vrt.h
// buildAlignedVrt): each window is a VRT that reads its input when the merge reads it, so no band
// is copied, and its pixels are the input's. The merge of such bands writes the windows.

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "exceptions.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "merge_crop.h"
#include "merge_multispectral.h"
#include "merge_vrt.h"

namespace
{
    using namespace ddb;

    // A capture of single-band rasters without a geotransform (the bands of a multispectral camera),
    // each shifted against the first by its (dx, dy) pixels
    struct Capture
    {
        const char *name;
        GDALDataType type;
        int width;
        int height;
        std::vector<std::pair<double, double>> shifts;
        std::optional<double> nodata;
    };

    const std::vector<Capture> kCaptures = {
        {"two_uint16_right_down", GDT_UInt16, 64, 48, {{0, 0}, {6, 4}}, std::nullopt},
        {"three_byte_left_up", GDT_Byte, 50, 33, {{0, 0}, {-7, -3}, {5, -2}}, std::nullopt},
        {"four_int16_nodata", GDT_Int16, 81, 59, {{3, 3}, {0, 0}, {-4, 1}, {1, -5}}, -32768.0},
        {"five_uint16_rounded", GDT_UInt16, 128, 96, {{0, 0}, {2.4, -3.6}, {-5, 3}, {0.5, 1.5}, {-0.4, 0.4}}, std::nullopt},
        {"six_float32_partial", GDT_Float32, 40, 30, {{0, 0}, {3, -2}, {-3, 2}, {2, 2}, {-2, -2}, {0, 1}}, -9999.0},
        {"seven_byte_one_pixel", GDT_Byte, 25, 13, {{0, 0}, {1, 0}, {0, 1}, {-1, 0}, {0, -1}, {1, -1}, {-1, 1}}, std::nullopt},
    };

    double valueAt(int band, int x, int y)
    {
        return static_cast<double>((x * 13 + y * 7 + band * 101) % 120) + 3.0;
    }

    std::vector<std::string> writeCapture(TestArea &area, const Capture &capture)
    {
        std::vector<std::string> paths;
        for (size_t band = 0; band < capture.shifts.size(); band++)
        {
            const fs::path path = area.getPath(std::string(capture.name) + "_" + std::to_string(band) + ".tif");
            const auto dataset = adoptDataset(GDALCreate(GDALGetDriverByName("GTiff"), path.string().c_str(),
                                                         capture.width, capture.height, 1, capture.type, nullptr));
            EXPECT_TRUE(dataset);
            GDALRasterBandH raster = GDALGetRasterBand(datasetHandle(dataset), 1);
            std::vector<double> row(static_cast<size_t>(capture.width));
            for (int y = 0; y < capture.height; y++)
            {
                for (int x = 0; x < capture.width; x++)
                    row[static_cast<size_t>(x)] = x + y < 4 && capture.nodata ? *capture.nodata
                                                                              : valueAt(static_cast<int>(band), x, y);
                EXPECT_EQ(GDALRasterIO(raster, GF_Write, 0, y, capture.width, 1, row.data(), capture.width, 1,
                                       GDT_Float64, 0, 0),
                          CE_None);
            }
            if (capture.nodata)
                GDALSetRasterNoDataValue(raster, *capture.nodata);
            paths.push_back(path.string());
        }
        return paths;
    }

    AlignmentCrop cropOf(const Capture &capture)
    {
        std::vector<BandAlignmentInfo> bands(capture.shifts.size());
        for (size_t i = 0; i < bands.size(); i++)
        {
            bands[i].detected = true;
            bands[i].shiftX = capture.shifts[i].first;
            bands[i].shiftY = capture.shifts[i].second;
            bands[i].imageWidth = capture.width;
            bands[i].imageHeight = capture.height;
        }
        const auto crop = planAlignmentCrop(bands, capture.width, capture.height);
        EXPECT_TRUE(crop.has_value());
        return crop.value_or(AlignmentCrop());
    }

    // The pixels of a window of a band, as doubles
    std::vector<double> readWindow(GDALRasterBandH band, int x, int y, int width, int height)
    {
        std::vector<double> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));
        EXPECT_EQ(GDALRasterIO(band, GF_Read, x, y, width, height, pixels.data(), width, height, GDT_Float64, 0, 0),
                  CE_None);
        return pixels;
    }

    size_t vsimemFiles()
    {
        const CPLStringList files(VSIReadDirRecursive("/vsimem/"));
        return static_cast<size_t>(files.Count());
    }

    // Each band of the aligned VRT is the window of its input the crop names, of its type and
    // nodata, read from the input: no band is copied to a file of its own (in /vsimem/ or
    // anywhere), the VRT of the merge is the one file
    TEST(MergeAlignedVrt, eachBandIsAWindowOfItsInputThatCopiesNoPixel)
    {
        TestArea area(TEST_NAME, true);
        for (const Capture &capture : kCaptures)
        {
            SCOPED_TRACE(capture.name);
            const std::vector<std::string> paths = writeCapture(area, capture);
            const AlignmentCrop crop = cropOf(capture);

            const size_t filesBefore = vsimemFiles();
            merge::SeparateVrt aligned = merge::buildAlignedVrt(paths, crop);
            EXPECT_EQ(vsimemFiles(), filesBefore + 1);

            GDALDatasetH vrt = aligned.handle();
            ASSERT_EQ(GDALGetRasterCount(vrt), static_cast<int>(paths.size()));
            EXPECT_EQ(GDALGetRasterXSize(vrt), crop.outW);
            EXPECT_EQ(GDALGetRasterYSize(vrt), crop.outH);
            ASSERT_EQ(aligned.sources.size(), paths.size());
            for (size_t i = 0; i < paths.size(); i++)
            {
                SCOPED_TRACE(i);
                EXPECT_STREQ(GDALGetDriverShortName(GDALGetDatasetDriver(datasetHandle(aligned.sources[i]))), "VRT");

                GDALRasterBandH band = GDALGetRasterBand(vrt, static_cast<int>(i) + 1);
                EXPECT_EQ(GDALGetRasterDataType(band), capture.type);
                int hasNodata = 0;
                const double nodata = GDALGetRasterNoDataValue(band, &hasNodata);
                EXPECT_EQ(hasNodata != 0, capture.nodata.has_value());
                if (capture.nodata)
                {
                    EXPECT_EQ(nodata, *capture.nodata);
                }

                const auto input = adoptDataset(GDALOpen(paths[i].c_str(), GA_ReadOnly));
                ASSERT_TRUE(input);
                const std::vector<double> expected =
                    readWindow(GDALGetRasterBand(datasetHandle(input), 1), crop.padLeft - crop.dx[i],
                               crop.padTop - crop.dy[i], crop.outW, crop.outH);
                EXPECT_EQ(readWindow(band, 0, 0, crop.outW, crop.outH), expected);
            }
            aligned.close();
            EXPECT_EQ(vsimemFiles(), filesBefore);
        }
    }

    // A window that does not lie inside its input is refused before anything is read
    TEST(MergeAlignedVrt, aWindowOutsideItsInputIsRefused)
    {
        TestArea area(TEST_NAME, true);
        const Capture &capture = kCaptures.front();
        const std::vector<std::string> paths = writeCapture(area, capture);
        const size_t filesBefore = vsimemFiles();
        for (const auto &[dx, dy] : {std::pair(1, 0), std::pair(0, -5), std::pair(-64, 0)})
        {
            AlignmentCrop crop = cropOf(capture);
            crop.dx[1] += dx;
            crop.dy[1] += dy;
            EXPECT_THROW(merge::buildAlignedVrt(paths, crop), GDALException);
        }
        EXPECT_EQ(vsimemFiles(), filesBefore);
        EXPECT_THROW(merge::buildAlignedVrt({paths[0], area.getPath("missing.tif").string()}, cropOf(capture)),
                     GDALException);
    }

    // The windows outlive nothing they read: the inputs close after the VRTs, whichever closes
    // the aligned VRT (close() or its destructor), and the merged VRT can be read to the end
    TEST(MergeAlignedVrt, theInputsCloseAfterTheWindowsThatReadThem)
    {
        TestArea area(TEST_NAME, true);
        const Capture &capture = kCaptures[3];
        const std::vector<std::string> paths = writeCapture(area, capture);
        const AlignmentCrop crop = cropOf(capture);
        std::vector<double> first;
        {
            merge::SeparateVrt aligned = merge::buildAlignedVrt(paths, crop);
            ASSERT_EQ(aligned.inputs.size(), paths.size());
            first = readWindow(GDALGetRasterBand(aligned.handle(), 1), 0, 0, crop.outW, crop.outH);
        }
        merge::SeparateVrt again = merge::buildAlignedVrt(paths, crop);
        merge::SeparateVrt moved(std::move(again));
        EXPECT_EQ(readWindow(GDALGetRasterBand(moved.handle(), 1), 0, 0, crop.outW, crop.outH), first);
        moved.close();
        EXPECT_TRUE(moved.inputs.empty());
    }
}
