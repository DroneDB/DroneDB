/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A JPEG 2000 builds the same COG as a GeoTIFF of the same pixels, and its decoding stays
// within the threads of the build.

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>

#include "gtest/gtest.h"

#include "build_budget.h"
#include "cog.h"
#include "cog_utils.h"
#include "gdal_inc.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"

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

    TEST(CogJpeg2000, onlyTheJpeg2000DriverDecodesWithItsOwnThreads)
    {
        EXPECT_TRUE(decodesWithOwnThreads("JP2OpenJPEG"));
        EXPECT_FALSE(decodesWithOwnThreads("GTiff"));
        EXPECT_FALSE(decodesWithOwnThreads("COG"));
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

    // The JPEG 2000 decoder starts one thread per CPU unless GDAL_NUM_THREADS says otherwise, and
    // the warp's -multi reads on a GDAL worker thread that a thread-local option does not reach:
    // a build capped to one thread peaked at about the CPU count before the decoding was capped
    TEST(CogJpeg2000, decodingStaysWithinTheThreadsOfTheBuild)
    {
        const unsigned cpus = std::thread::hardware_concurrency();
        if (cpus < 8)
            GTEST_SKIP() << "needs 8 CPUs to tell a capped decoder from one thread per CPU (" << cpus << ")";

        TestArea ta(TEST_NAME, true);
        auto spec = patternSpec(2048);
        // Small code blocks: the decoder loads many blocks per read, one thread each
        const auto source = testutils::createGeoRaster(ta.getPath("source.tif"), spec);
        GDALDatasetH src = GDALOpen(source.string().c_str(), GA_ReadOnly);
        ASSERT_NE(src, nullptr);
        char **opts = CSLSetNameValue(nullptr, "REVERSIBLE", "YES");
        opts = CSLSetNameValue(opts, "BLOCKXSIZE", "256");
        opts = CSLSetNameValue(opts, "BLOCKYSIZE", "256");
        GDALDatasetH jp2 = GDALCreateCopy(GDALGetDriverByName("JP2OpenJPEG"), ta.getPath("ortho.jp2").string().c_str(),
                                          src, FALSE, opts, nullptr, nullptr);
        CSLDestroy(opts);
        GDALClose(src);
        ASSERT_NE(jp2, nullptr);
        GDALClose(jp2);

        const int baseline = processThreads();
        ASSERT_GT(baseline, 0);
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

        buildCog(ta.getPath("ortho.jp2").string(), ta.getPath("cog.tif").string(), BuildOptions::withThreads(1));
        done.store(true);
        sampler.join();

        // The sampler itself, OpenJPEG's one worker and some slack; not one per CPU
        EXPECT_LE(peak.load() - baseline, 4) << "peak " << peak.load() << " threads, baseline " << baseline
                                             << ", " << cpus << " CPUs";
    }
#endif
} // namespace
