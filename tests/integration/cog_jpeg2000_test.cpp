/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A JPEG 2000 builds the same COG as a GeoTIFF of the same pixels, and the decoding of a source
// stays within the threads of the build: a JPEG 2000, whose decoder has threads of its own, and a
// compressed GeoTIFF, whose blocks GDAL decodes on the threads of the build.

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include "gtest/gtest.h"

#include "build_budget.h"
#include "cog.h"
#include "cog_utils.h"
#include "gdal_inc.h"
#include "in_process_raster_helper.h"
#include "raster_helper.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "user_open_child.h"

namespace
{
    using namespace ddb;

    testutils::GeoRasterSpec patternSpec(int size)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = size;
        spec.originX = 1000000.0;
        spec.originY = 5700000.0;
        spec.pixelSize = 0.05;
        spec.value = [](int band, int x, int y) { return static_cast<double>((x * 7 + y * 3 + band * 50) % 251); };
        return spec;
    }

    struct CogShape
    {
        int width = 0;
        int height = 0;
        int bands = 0;
        double geotransform[6] = {};
        std::vector<int> checksums;
    };

    CogShape readCog(const fs::path &path)
    {
        CogShape shape;
        GDALDatasetH ds = GDALOpen(path.string().c_str(), GA_ReadOnly);
        if (!ds) return shape;
        shape.width = GDALGetRasterXSize(ds);
        shape.height = GDALGetRasterYSize(ds);
        shape.bands = GDALGetRasterCount(ds);
        GDALGetGeoTransform(ds, shape.geotransform);
        for (int b = 1; b <= shape.bands; b++)
            shape.checksums.push_back(GDALChecksumImage(GDALGetRasterBand(ds, b), 0, 0, shape.width, shape.height));
        GDALClose(ds);
        return shape;
    }

    TEST(CogJpeg2000, gdalIsBuiltWithOpenJpeg)
    {
        EXPECT_NE(GDALGetDriverByName("JP2OpenJPEG"), nullptr) << "GDAL is built without OpenJPEG";
    }

    TEST(CogJpeg2000, buildsTheSameCogAsTheGeoTiffOfItsPixels)
    {
        TestArea ta(TEST_NAME, true);
        const auto spec = patternSpec(512);
        const auto tif = testutils::createGeoRaster(ta.getPath("ortho.tif"), spec);
        const auto jp2 = testutils::createGeoJp2(ta.getPath("ortho.jp2"), spec);

        buildCog(tif.string(), ta.getPath("from-tif.tif").string(), BuildOptions::withThreads(2));
        buildCog(jp2.string(), ta.getPath("from-jp2.tif").string(), BuildOptions::withThreads(2));

        const CogShape fromTif = readCog(ta.getPath("from-tif.tif"));
        const CogShape fromJp2 = readCog(ta.getPath("from-jp2.tif"));
        ASSERT_GT(fromTif.width, 0);
        EXPECT_EQ(fromJp2.width, fromTif.width);
        EXPECT_EQ(fromJp2.height, fromTif.height);
        EXPECT_EQ(fromJp2.bands, fromTif.bands);
        for (int i = 0; i < 6; i++)
            EXPECT_DOUBLE_EQ(fromJp2.geotransform[i], fromTif.geotransform[i]) << i;
        EXPECT_EQ(fromJp2.checksums, fromTif.checksums) << "a lossless JPEG 2000 has the pixels of its GeoTIFF";
    }

    // The creation options of a tiled GeoTIFF compressed with DEFLATE, for blockCopy
    char **deflateTiled()
    {
        return CSLSetNameValue(CSLSetNameValue(nullptr, "TILED", "YES"), "COMPRESS", "DEFLATE");
    }

    // A copy of `source` made by `driver` with 256 x 256 blocks: the decoder loads many blocks per
    // read, which it may decode on one thread each
    fs::path blockCopy(const fs::path &source, const fs::path &out, const char *driver, char **options)
    {
        GDALDatasetH src = GDALOpen(source.string().c_str(), GA_ReadOnly);
        EXPECT_NE(src, nullptr);
        options = CSLSetNameValue(options, "BLOCKXSIZE", "256");
        options = CSLSetNameValue(options, "BLOCKYSIZE", "256");
        GDALDatasetH copy = GDALCreateCopy(GDALGetDriverByName(driver), out.string().c_str(), src, FALSE, options,
                                           nullptr, nullptr);
        CSLDestroy(options);
        GDALClose(src);
        EXPECT_NE(copy, nullptr);
        GDALClose(copy);
        return out;
    }

    // A compressed GeoTIFF decodes its blocks on as many threads as the build holds: the COG of a
    // build of 8 threads has the pixels of the COG of a build of one
    TEST(CogDecoding, theThreadsOfTheBuildDoNotChangeThePixels)
    {
        TestArea ta(TEST_NAME, true);
        const auto source = testutils::createGeoRaster(ta.getPath("source.tif"), patternSpec(2048));
        const auto deflate = blockCopy(source, ta.getPath("deflate.tif"), "GTiff", deflateTiled());
        buildCog(deflate.string(), ta.getPath("one.tif").string(), BuildOptions::withThreads(1));
        buildCog(deflate.string(), ta.getPath("eight.tif").string(), BuildOptions::withThreads(8));
        const CogShape one = readCog(ta.getPath("one.tif"));
        ASSERT_GT(one.width, 0);
        EXPECT_EQ(readCog(ta.getPath("eight.tif")).checksums, one.checksums);
    }

#ifdef __linux__
    // The threads of this process, from /proc/self/status
    int processThreads()
    {
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line))
            if (line.rfind("Threads:", 0) == 0)
                return std::stoi(line.substr(8));
        return -1;
    }

    // The most threads this process had while `run` ran, above the threads it had before
    int extraThreadsDuring(const std::function<void()> &run)
    {
        const int baseline = processThreads();
        EXPECT_GT(baseline, 0);
        std::atomic<bool> done{false};
        std::atomic<int> peak{baseline};
        std::thread sampler([&] {
            while (!done.load())
            {
                const int now = processThreads();
                if (now > peak.load()) peak.store(now);
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
        run();
        done.store(true);
        sampler.join();
        return peak.load() - baseline;
    }

    // A build capped to one thread, of a source GDAL could decode on one thread per CPU: the
    // sampler itself, the decoder's one worker and some slack, not one per CPU. The build runs in
    // this process (an in-process raster helper, in the child that may open files of users), so
    // its threads are the threads this process counts.
    void expectDecodingWithinOneThread(const fs::path &source, const fs::path &cog)
    {
        testutils::InProcessRasterHelper helper;
        const raster::ScopedRasterHelper installed(helper);
        const unsigned cpus = std::thread::hardware_concurrency();
        const int extra = extraThreadsDuring(
            [&] { buildCog(source.string(), cog.string(), BuildOptions::withThreads(1)); });
        EXPECT_LE(extra, 4) << extra << " threads above the baseline, " << cpus << " CPUs";
    }

    // The JPEG 2000 decoder starts one thread per CPU unless GDAL_NUM_THREADS says otherwise, and
    // a warp with -multi reads on a GDAL worker thread that a thread-local option does not reach:
    // a build capped to one thread peaked at about the CPU count before the decoding was capped
    USER_OPEN_TEST(CogJpeg2000, decodingStaysWithinTheThreadsOfTheBuild)
    {
        if (std::thread::hardware_concurrency() < 8)
            GTEST_SKIP() << "needs 8 CPUs to tell a capped decoder from one thread per CPU";

        TestArea ta(TEST_NAME, true);
        const auto source = testutils::createGeoRaster(ta.getPath("source.tif"), patternSpec(2048));
        const auto jp2 = blockCopy(source, ta.getPath("ortho.jp2"), "JP2OpenJPEG",
                                   CSLSetNameValue(nullptr, "REVERSIBLE", "YES"));
        expectDecodingWithinOneThread(jp2, ta.getPath("cog.tif"));
    }

    // A compressed GeoTIFF decodes its blocks on the threads of the build (GDAL_NUM_THREADS of
    // the reading thread), so a build capped to one thread decodes on one
    USER_OPEN_TEST(CogDecoding, aCompressedGeoTiffStaysWithinTheThreadsOfTheBuild)
    {
        if (std::thread::hardware_concurrency() < 8)
            GTEST_SKIP() << "needs 8 CPUs to tell a capped decoder from one thread per CPU";

        TestArea ta(TEST_NAME, true);
        const auto source = testutils::createGeoRaster(ta.getPath("source.tif"), patternSpec(2048));
        const auto deflate = blockCopy(source, ta.getPath("deflate.tif"), "GTiff", deflateTiled());
        expectDecodingWithinOneThread(deflate, ta.getPath("cog.tif"));
    }

#endif
} // namespace
