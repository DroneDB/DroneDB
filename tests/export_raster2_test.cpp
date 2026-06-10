/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* Tests for DDBExportRaster2: block-windowed raster export with progress
 * reporting and cooperative cancellation (Processing Platform, Sprint 1). */

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "ddb.h"
#include "fs.h"
#include "gdal_inc.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

namespace {

// Returns the current resident set size (working set) of this process in bytes.
size_t currentWorkingSetBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<size_t>(pmc.WorkingSetSize);
    return 0;
#else
    long rssPages = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (f) {
        long total = 0;
        if (std::fscanf(f, "%ld %ld", &total, &rssPages) != 2) rssPages = 0;
        std::fclose(f);
    }
    return static_cast<size_t>(rssPages) * static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}

// Creates a synthetic multi-band Float32 GeoTIFF with smooth gradients so that
// vegetation formulas (e.g. NDVI from bands R,N) produce a varying result.
fs::path makeSyntheticRaster(const fs::path& path, int width, int height, int bands) {
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    char** opts = nullptr;
    opts = CSLAddString(opts, "TILED=YES");
    opts = CSLAddString(opts, "BLOCKXSIZE=256");
    opts = CSLAddString(opts, "BLOCKYSIZE=256");
    opts = CSLAddString(opts, "COMPRESS=DEFLATE");
    opts = CSLAddString(opts, "BIGTIFF=IF_SAFER");

    GDALDatasetH ds = GDALCreate(drv, path.string().c_str(), width, height, bands,
                                 GDT_Float32, opts);
    CSLDestroy(opts);
    if (!ds) throw std::runtime_error("Cannot create synthetic raster");

    // A trivial geotransform + projection so the export preserves georeferencing.
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    GDALSetGeoTransform(ds, gt);
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    char* wkt = nullptr;
    srs.exportToWkt(&wkt);
    GDALSetProjection(ds, wkt);
    CPLFree(wkt);

    std::vector<float> row(static_cast<size_t>(width));
    for (int b = 0; b < bands; b++) {
        GDALRasterBandH band = GDALGetRasterBand(ds, b + 1);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                // Band 0 (R) low, band 1 (N) high near the top so NDVI varies.
                const float base = static_cast<float>((x + y) % 251) / 251.0f;
                row[static_cast<size_t>(x)] =
                    100.0f + base * 100.0f + static_cast<float>(b) * 80.0f;
            }
            GDALRasterIO(band, GF_Write, 0, y, width, 1, row.data(), width, 1,
                         GDT_Float32, 0, 0);
        }
    }
    GDALFlushCache(ds);
    GDALClose(ds);
    return path;
}

// Progress callback context.
struct ProgressState {
    std::vector<double> fractions;
    bool cancel = false;
    double cancelAtFraction = -1.0;
};

int progressCallback(double fraction, const char* /*phase*/, void* userData) {
    auto* st = static_cast<ProgressState*>(userData);
    st->fractions.push_back(fraction);
    if (st->cancelAtFraction >= 0.0 && fraction >= st->cancelAtFraction) return 1;
    return st->cancel ? 1 : 0;
}

class ExportRaster2Test : public ::testing::Test {
 protected:
    void SetUp() override {
        DDBRegisterProcess(false);
        // Keep GDAL's block cache modest so the memory test isolates our buffers.
        prevCacheMax_ = GDALGetCacheMax64();
        GDALSetCacheMax(64 * 1024 * 1024);
    }

    void TearDown() override {
        GDALSetCacheMax64(prevCacheMax_);
    }

 private:
    GIntBig prevCacheMax_ = 0;
};

// 1) Memory: a large raster must not be loaded whole-into-memory.
// Disabled by default: generates a very large synthetic file (~800 MB) that is
// slow and I/O-heavy in CI. Run explicitly with --gtest_filter=*MemoryBounded.
MANUAL_TEST_F(ExportRaster2Test, MemoryBounded) {
    TestArea ta("ExportRaster2_Memory", true);
    const fs::path input = ta.getPath("big.tif");
    const fs::path output = ta.getPath("big_ndvi.tif");

    // 10000x10000, 2 bands (R, N) Float32.
    makeSyntheticRaster(input, 10000, 10000, 2);

    const size_t baseline = currentWorkingSetBytes();
    std::atomic<size_t> peak{baseline};
    std::atomic<bool> running{true};
    std::thread sampler([&]() {
        while (running.load()) {
            const size_t cur = currentWorkingSetBytes();
            size_t prev = peak.load();
            while (cur > prev && !peak.compare_exchange_weak(prev, cur)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    const DDBErr err = DDBExportRaster2(input.string().c_str(), output.string().c_str(),
                                        nullptr, nullptr, "NDVI", "RN", "rdylgn",
                                        nullptr, 512, nullptr, nullptr);
    running.store(false);
    sampler.join();

    ASSERT_EQ(err, DDBERR_NONE) << DDBGetLastError();
    ASSERT_TRUE(fs::exists(output));

    const size_t deltaBytes = peak.load() > baseline ? peak.load() - baseline : 0;
    const double deltaMb = static_cast<double>(deltaBytes) / (1024.0 * 1024.0);
    // Whole-image processing of this raster would require >1 GB; windowed
    // processing must stay well under the 600 MB host budget (roadmap DoD).
    EXPECT_LT(deltaMb, 600.0) << "Peak memory delta was " << deltaMb << " MB";
}

// 2) Equivalence: tiling must not change the result. A single-tile export and
//    a many-tiles export of the same raster must be pixel-identical.
TEST_F(ExportRaster2Test, TilingEquivalence) {
    TestArea ta("ExportRaster2_Equivalence", true);
    const fs::path input = ta.getPath("small.tif");
    const fs::path outFull = ta.getPath("full.tif");
    const fs::path outTiled = ta.getPath("tiled.tif");

    makeSyntheticRaster(input, 512, 512, 2);

    ASSERT_EQ(DDBExportRaster2(input.string().c_str(), outFull.string().c_str(),
                               nullptr, nullptr, "NDVI", "RN", "rdylgn", nullptr,
                               512, nullptr, nullptr), DDBERR_NONE);
    ASSERT_EQ(DDBExportRaster2(input.string().c_str(), outTiled.string().c_str(),
                               nullptr, nullptr, "NDVI", "RN", "rdylgn", nullptr,
                               64, nullptr, nullptr), DDBERR_NONE);

    GDALDatasetH a = GDALOpen(outFull.string().c_str(), GA_ReadOnly);
    GDALDatasetH b = GDALOpen(outTiled.string().c_str(), GA_ReadOnly);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_EQ(GDALGetRasterCount(a), 4);
    ASSERT_EQ(GDALGetRasterCount(b), 4);

    const int w = GDALGetRasterXSize(a), h = GDALGetRasterYSize(a);
    std::vector<uint8_t> bufA(static_cast<size_t>(w) * h);
    std::vector<uint8_t> bufB(bufA.size());
    for (int band = 1; band <= 4; band++) {
        GDALRasterIO(GDALGetRasterBand(a, band), GF_Read, 0, 0, w, h, bufA.data(),
                     w, h, GDT_Byte, 0, 0);
        GDALRasterIO(GDALGetRasterBand(b, band), GF_Read, 0, 0, w, h, bufB.data(),
                     w, h, GDT_Byte, 0, 0);
        EXPECT_EQ(bufA, bufB) << "Band " << band << " differs between tilings";
    }
    GDALClose(a);
    GDALClose(b);
}

// 3) Cancel: returning non-zero from the callback aborts and removes output.
TEST_F(ExportRaster2Test, Cancel) {
    TestArea ta("ExportRaster2_Cancel", true);
    const fs::path input = ta.getPath("cancel.tif");
    const fs::path output = ta.getPath("cancel_out.tif");

    makeSyntheticRaster(input, 2048, 2048, 2);

    ProgressState st;
    st.cancelAtFraction = 0.3;

    const DDBErr err = DDBExportRaster2(input.string().c_str(), output.string().c_str(),
                                        nullptr, nullptr, "NDVI", "RN", "rdylgn",
                                        nullptr, 128, progressCallback, &st);

    EXPECT_EQ(err, DDBERR_CANCELED);
    EXPECT_FALSE(fs::exists(output)) << "Canceled export must not leave an output file";
}

// 4) Auto tile size and clamping: tileSize 0 (auto), tiny, and large all work.
TEST_F(ExportRaster2Test, AutoAndClampedTileSize) {
    TestArea ta("ExportRaster2_TileSize", true);
    const fs::path input = ta.getPath("ts.tif");
    makeSyntheticRaster(input, 600, 480, 2);

    for (int tileSize : {0, 1, 5000}) {
        const fs::path output = ta.getPath("ts_out_" + std::to_string(tileSize) + ".tif");
        const DDBErr err = DDBExportRaster2(input.string().c_str(), output.string().c_str(),
                                            nullptr, nullptr, "NDVI", "RN", "rdylgn",
                                            nullptr, tileSize, nullptr, nullptr);
        ASSERT_EQ(err, DDBERR_NONE) << "tileSize=" << tileSize << ": " << DDBGetLastError();
        GDALDatasetH ds = GDALOpen(output.string().c_str(), GA_ReadOnly);
        ASSERT_NE(ds, nullptr);
        EXPECT_EQ(GDALGetRasterXSize(ds), 600);
        EXPECT_EQ(GDALGetRasterYSize(ds), 480);
        EXPECT_EQ(GDALGetRasterCount(ds), 4);
        GDALClose(ds);
    }
}

// 5) Progress is monotonic non-decreasing and reaches 1.0 on success.
TEST_F(ExportRaster2Test, ProgressMonotonic) {
    TestArea ta("ExportRaster2_Progress", true);
    const fs::path input = ta.getPath("prog.tif");
    const fs::path output = ta.getPath("prog_out.tif");
    makeSyntheticRaster(input, 1500, 1500, 2);

    ProgressState st;
    const DDBErr err = DDBExportRaster2(input.string().c_str(), output.string().c_str(),
                                        nullptr, nullptr, "NDVI", "RN", "rdylgn",
                                        nullptr, 256, progressCallback, &st);

    ASSERT_EQ(err, DDBERR_NONE) << DDBGetLastError();
    ASSERT_FALSE(st.fractions.empty());
    for (size_t i = 1; i < st.fractions.size(); i++) {
        EXPECT_GE(st.fractions[i], st.fractions[i - 1])
            << "Progress decreased at index " << i;
    }
    EXPECT_DOUBLE_EQ(st.fractions.back(), 1.0);
}

// 6) Auto-percentile rescale path (formula without a defined range) must stay
//    memory-bounded thanks to reservoir subsampling. vNDVI has hasRange=false.
// Disabled by default: generates a very large synthetic file (~768 MB) that is
// slow and I/O-heavy in CI. Run explicitly with --gtest_filter=*AutoPercentile*.
MANUAL_TEST_F(ExportRaster2Test, AutoPercentileMemoryBounded) {
    TestArea ta("ExportRaster2_AutoPercentile", true);
    const fs::path input = ta.getPath("ap.tif");
    const fs::path output = ta.getPath("ap_out.tif");

    // vNDVI needs RGB bands.
    makeSyntheticRaster(input, 8000, 8000, 3);

    const size_t baseline = currentWorkingSetBytes();
    std::atomic<size_t> peak{baseline};
    std::atomic<bool> running{true};
    std::thread sampler([&]() {
        while (running.load()) {
            const size_t cur = currentWorkingSetBytes();
            size_t prev = peak.load();
            while (cur > prev && !peak.compare_exchange_weak(prev, cur)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    const DDBErr err = DDBExportRaster2(input.string().c_str(), output.string().c_str(),
                                        nullptr, nullptr, "vNDVI", "RGB", "rdylgn",
                                        nullptr, 512, nullptr, nullptr);
    running.store(false);
    sampler.join();

    ASSERT_EQ(err, DDBERR_NONE) << DDBGetLastError();
    ASSERT_TRUE(fs::exists(output));

    const size_t deltaBytes = peak.load() > baseline ? peak.load() - baseline : 0;
    const double deltaMb = static_cast<double>(deltaBytes) / (1024.0 * 1024.0);
    // Collecting every valid pixel would need >2 GB here; reservoir subsampling
    // must keep the auto-percentile pass well under the host budget.
    EXPECT_LT(deltaMb, 600.0) << "Peak memory delta was " << deltaMb << " MB";
}

}  // namespace
