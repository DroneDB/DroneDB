/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Regression tests for latent bugs:
//
//  negativeOnlyRasterStretch    globalMax must start at numeric_limits::lowest().
//  zeroHeight*DoesNotCrash      fingerprint() must not divide by a zero height.
//  generatesWithoutGdalWarnings  no WEBP WRITE_EXIF_METADATA warning; run under
//        ASan to also catch the alloc-dealloc mismatch.
//  registerProcessKeepsHostSignalHandlers  DDBRegisterProcess must not replace the
//        host's SIGSEGV/SIGFPE/SIGTERM/SIGINT handlers (the .NET Registry relies on them).

#include <cstdlib>
#include <fstream>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "gtest/gtest.h"

#include "entry.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "gdaltiler.h"
#include "mio.h"
#include "pointcloud.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"
#include "thumbs.h"

namespace
{
    using namespace ddb;

    // Int16 raster in EPSG:3857 (no warp needed) whose values are all negative.
    fs::path createNegativeRaster(const fs::path &path, int w, int h)
    {
        GDALDriverH drv = GDALGetDriverByName("GTiff");
        GDALDatasetH ds = GDALCreate(drv, path.string().c_str(), w, h, 1, GDT_Int16, nullptr);
        // 1 m pixels near Null Island in Web Mercator
        double gt[6] = {1000.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
        GDALSetGeoTransform(ds, gt);
        OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(srs, 3857);
        char *wkt = nullptr;
        OSRExportToWkt(srs, &wkt);
        GDALSetProjection(ds, wkt);
        CPLFree(wkt);
        OSRDestroySpatialReference(srs);
        std::vector<int16_t> row(w);
        GDALRasterBandH band = GDALGetRasterBand(ds, 1);
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
                row[x] = static_cast<int16_t>(-500 + (400 * x) / std::max(1, w - 1)); // -500 .. -100
            (void)GDALRasterIO(band, GF_Write, 0, y, w, 1, row.data(), w, 1, GDT_Int16, 0, 0);
        }
        GDALFlushCache(ds);
        GDALClose(ds);
        return path;
    }

    // Decode an in-memory PNG and return the max value of band 1.
    int pngMaxBand1(const uint8_t *buf, int size)
    {
        const std::string vsi = "/vsimem/ddb_latent_" + std::to_string(reinterpret_cast<uintptr_t>(buf)) + ".png";
        VSIFCloseL(VSIFileFromMemBuffer(vsi.c_str(), const_cast<uint8_t *>(buf), size, FALSE));
        GDALDatasetH ds = GDALOpen(vsi.c_str(), GA_ReadOnly);
        EXPECT_NE(ds, nullptr);
        const int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds);
        std::vector<uint8_t> px(static_cast<size_t>(w) * h), alpha(px.size());
        (void)GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, 0, 0, w, h, px.data(), w, h, GDT_Byte, 0, 0);
        (void)GDALRasterIO(GDALGetRasterBand(ds, GDALGetRasterCount(ds)), GF_Read, 0, 0, w, h, alpha.data(), w, h, GDT_Byte, 0, 0);
        int mx = 0;
        for (size_t i = 0; i < px.size(); i++)
            if (alpha[i] > 0) mx = std::max(mx, static_cast<int>(px[i]));
        GDALClose(ds);
        VSIUnlink(vsi.c_str());
        return mx;
    }

    TEST(TilerDefects, negativeOnlyRasterStretch)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = createNegativeRaster(ta.getPath("neg.tif"), 512, 512);
        GDALTiler t(raster.string(), "", 256);
        const int tz = t.getMinMaxZ().max;
        const auto b = t.getMinMaxCoordsForZ(tz);
        // The brightest valid pixel over all tiles must be stretched to (close to)
        // 255. With globalMax == numeric_limits<double>::min() the range is
        // [-500, ~0] and the max value -100 lands at ~204.
        int brightest = 0;
        for (int tx = b.min.x; tx <= b.max.x; tx++)
        {
            for (int ty = b.min.y; ty <= b.max.y; ty++)
            {
                uint8_t *buf = nullptr;
                int size = 0;
                t.tile(tz, tx, ty, &buf, &size);
                ASSERT_NE(buf, nullptr);
                brightest = std::max(brightest, pngMaxBand1(buf, size));
                VSIFree(buf);
            }
        }
        EXPECT_GE(brightest, 250);
    }

    TEST(FingerprintDefects, zeroHeightImageDoesNotCrash)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path jpg = ta.downloadTestAsset(
            testDataUrl("images/DJI_0027.JPG"), "DJI_0027.JPG");
        // Patch the SOF0 frame header height to 0 so Exiv2 reports pixelHeight()==0.
        std::ifstream in(jpg, std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
        in.close();
        bool patched = false;
        for (size_t i = 0; i + 9 < bytes.size(); i++)
        {
            if (bytes[i] == 0xFF && bytes[i + 1] == 0xC0) // SOF0: FF C0 len(2) prec(1) height(2) width(2)
            {
                bytes[i + 5] = 0;
                bytes[i + 6] = 0;
                patched = true;
                break;
            }
        }
        ASSERT_TRUE(patched);
        const fs::path broken = ta.getPath("zero_height.jpg");
        std::ofstream out(broken, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        out.close();

        EntryType type = EntryType::Undefined;
        EXPECT_NO_THROW(type = fingerprint(broken));
        EXPECT_NE(type, EntryType::Undefined);
    }

    // Exiv2 takes the dimensions from EXIF when present, so the SIGFPE needs a
    // JPEG without EXIF whose SOF0 height is 0 (truncated/stripped files).
    TEST(FingerprintDefects, zeroHeightExiflessJpegDoesNotCrash)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path jpg = ta.getPath("noexif.jpg");
        {
            GDALDatasetH mem = GDALCreate(GDALGetDriverByName("MEM"), "", 64, 32, 3, GDT_Byte, nullptr);
            ASSERT_NE(mem, nullptr);
            GDALDatasetH out = GDALCreateCopy(GDALGetDriverByName("JPEG"), jpg.string().c_str(), mem, FALSE,
                                              nullptr, nullptr, nullptr);
            ASSERT_NE(out, nullptr);
            GDALClose(out);
            GDALClose(mem);
        }
        std::ifstream in(jpg, std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
        in.close();
        bool patched = false;
        for (size_t i = 0; i + 9 < bytes.size(); i++)
        {
            if (bytes[i] == 0xFF && bytes[i + 1] == 0xC0)
            {
                bytes[i + 5] = 0;
                bytes[i + 6] = 0;
                patched = true;
                break;
            }
        }
        ASSERT_TRUE(patched);
        std::ofstream out(jpg, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        out.close();

        EntryType type = EntryType::Undefined;
        EXPECT_NO_THROW(type = fingerprint(jpg));
        EXPECT_EQ(type, EntryType::Image);
    }

    struct WarningCollector
    {
        static std::vector<std::string> warnings;
        static void CPL_STDCALL handler(CPLErr cls, CPLErrorNum, const char *msg)
        {
            if (cls == CE_Warning) warnings.emplace_back(msg);
        }
    };
    std::vector<std::string> WarningCollector::warnings;

    TEST(PointCloudThumbDefects, generatesWithoutGdalWarnings)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path laz = ta.downloadTestAsset(
            testDataUrl("brighton/point_cloud.laz"), "point_cloud.laz");
        buildCopc({laz.string()}, ta.getFolder("copc").string());
        const fs::path copc = ta.getFolder("copc") / "cloud.copc.laz";
        ASSERT_TRUE(fs::exists(copc));
        WarningCollector::warnings.clear();
        CPLPushErrorHandler(WarningCollector::handler);
        uint8_t *buf = nullptr;
        int size = 0;
        generateThumb(copc, 256, "", false, &buf, &size);   // -> generatePointCloudThumb; run under ASan for alloc-dealloc-mismatch
        CPLPopErrorHandler();
        ASSERT_NE(buf, nullptr);
        EXPECT_GT(size, 0);
        VSIFree(buf);
        for (const auto &w : WarningCollector::warnings)
            ADD_FAILURE() << "GDAL warning during thumbnail: " << w;  // WRITE_EXIF_METADATA on WEBP
    }
    TEST(SignalDefects, registerProcessKeepsHostSignalHandlers)
    {
        const fs::path probe = io::getExeFolderPath() / DDB_SIGNAL_PROBE_NAME;
        ASSERT_TRUE(fs::exists(probe)) << probe;
        const int rc = std::system(("\"" + probe.string() + "\"").c_str());
#ifdef _WIN32
        EXPECT_EQ(rc, 0) << "DDBRegisterProcess replaced the host's unhandled exception filter";
#else
        ASSERT_TRUE(WIFEXITED(rc)) << "probe did not exit normally: " << rc;
        EXPECT_EQ(WEXITSTATUS(rc), 0) << "DDBRegisterProcess replaced host signal handlers (see stderr)";
#endif
    }
} // namespace
