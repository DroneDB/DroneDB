/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The RAII resource layer (gdal_raii.h, io::ScopedTempDir) and the leaks it closes.

#include <stdexcept>
#include <string>

#include "align.h"
#include "exceptions.h"
#include "gdal_raii.h"
#include "mio.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    bool vsiExists(const std::string &path)
    {
        VSIStatBufL st;
        return VSIStatL(path.c_str(), &st) == 0;
    }

    void writeVsi(const std::string &path, const std::string &content)
    {
        const VsiFilePtr f(VSIFOpenL(path.c_str(), "wb"));
        ASSERT_TRUE(f);
        ASSERT_EQ(VSIFWriteL(content.data(), 1, content.size(), f.get()), content.size());
    }

    GDALDatasetUniquePtr memDataset()
    {
        return adoptDataset(GDALCreate(GDALGetDriverByName("MEM"), "", 4, 4, 1, GDT_Byte, nullptr));
    }

    TEST(Raii, VsiMemFileUnlinksOnScopeExit)
    {
        std::string path;
        {
            const VsiMemFile file = VsiMemFile::unique("raii-", ".bin");
            path = file.path();
            EXPECT_EQ(path.rfind("/vsimem/raii-", 0), 0u);
            EXPECT_EQ(path.substr(path.size() - 4), ".bin");
            writeVsi(path, "abc");
            EXPECT_TRUE(vsiExists(path));
        }
        EXPECT_FALSE(vsiExists(path));
    }

    TEST(Raii, VsiMemFileUnlinksOnThrow)
    {
        std::string path;
        EXPECT_THROW(
            {
                const VsiMemFile file = VsiMemFile::unique("raii-", ".bin");
                path = file.path();
                writeVsi(path, "abc");
                throw std::runtime_error("fail");
            },
            std::runtime_error);
        EXPECT_FALSE(vsiExists(path));
    }

    TEST(Raii, VsiMemFileReleaseAndMoveTransferOwnership)
    {
        VsiMemFile a = VsiMemFile::unique("raii-", ".bin");
        const std::string path = a.path();
        writeVsi(path, "abc");
        {
            const VsiMemFile b(std::move(a));
            EXPECT_EQ(b.path(), path);
            EXPECT_TRUE(a.path().empty());
        }
        EXPECT_FALSE(vsiExists(path)) << "the new owner removes it";

        VsiMemFile c = VsiMemFile::unique("raii-", ".bin");
        writeVsi(c.path(), "abc");
        const std::string kept = c.release();
        EXPECT_TRUE(c.path().empty());
        EXPECT_TRUE(vsiExists(kept)) << "released: kept";
        VSIUnlink(kept.c_str());
    }

    TEST(Raii, StealMemFileBufferTakesTheBytes)
    {
        VsiMemFile file = VsiMemFile::unique("raii-", ".bin");
        writeVsi(file.path(), "hello");

        int size = -1;
        const auto bytes = file.stealBuffer(size);
        ASSERT_TRUE(bytes);
        EXPECT_EQ(std::string(reinterpret_cast<const char *>(bytes.get()), static_cast<size_t>(size)),
                  "hello");
        EXPECT_FALSE(vsiExists(file.path())) << "the file is gone once its buffer is taken";

        vsi_l_offset again = 7;
        EXPECT_FALSE(stealMemFileBuffer(file.path(), again));
        EXPECT_EQ(again, 0u);
    }

    TEST(Raii, ScopedTempDirRemovesOnThrowAndKeepsWhenReleased)
    {
        TestArea ta(TEST_NAME, true);
        fs::path removed;
        EXPECT_THROW(
            {
                const io::ScopedTempDir dir = io::ScopedTempDir::create(ta.getFolder(), "tmp-");
                removed = dir.path();
                ASSERT_TRUE(fs::is_directory(removed));
                writeFile(removed / "sub" / "a.txt", "a");
                throw std::runtime_error("fail");
            },
            std::runtime_error);
        EXPECT_FALSE(fs::exists(removed));

        fs::path kept;
        {
            io::ScopedTempDir dir = io::ScopedTempDir::create(ta.getFolder(), "tmp-");
            kept = dir.release();
            EXPECT_TRUE(dir.empty());
        }
        EXPECT_TRUE(fs::is_directory(kept));

        // A single file, adopted before it exists; reset removes the previous path
        const fs::path file = ta.getPath("partial.tif");
        io::ScopedTempDir guard(file);
        writeFile(file, "x");
        guard.reset(kept);
        EXPECT_FALSE(fs::exists(file));
        io::ScopedTempDir moved(std::move(guard));
        EXPECT_TRUE(guard.empty());
        moved.reset();
        EXPECT_FALSE(fs::exists(kept));
    }

    TEST(Raii, ScopedThreadConfigOptionRestoresThePreviousValue)
    {
        const char *key = "DDB_TEST_OPTION";
        ASSERT_EQ(CPLGetThreadLocalConfigOption(key, nullptr), nullptr);
        {
            const ScopedThreadConfigOption outer(key, "outer");
            EXPECT_STREQ(CPLGetThreadLocalConfigOption(key, nullptr), "outer");
            {
                const ScopedThreadConfigOption inner(key, "inner");
                EXPECT_STREQ(CPLGetThreadLocalConfigOption(key, nullptr), "inner");
            }
            EXPECT_STREQ(CPLGetThreadLocalConfigOption(key, nullptr), "outer");
        }
        EXPECT_EQ(CPLGetThreadLocalConfigOption(key, nullptr), nullptr);
    }

    TEST(Raii, DatasetsCloseWithTheirOwner)
    {
        const int before = openDatasetCount();
        {
            const auto ds = memDataset();
            ASSERT_TRUE(ds);
            EXPECT_EQ(openDatasetCount(), before + 1);
            EXPECT_EQ(datasetHandles({}).size(), 0u);
        }
        EXPECT_EQ(openDatasetCount(), before);

        auto ds = memDataset();
        ASSERT_TRUE(ds);
        EXPECT_EQ(closeDataset(std::move(ds)), CE_None);
        EXPECT_FALSE(ds);
        EXPECT_EQ(openDatasetCount(), before);
        EXPECT_EQ(closeDataset(nullptr), CE_None);
    }

    TEST(Raii, SrsFactoriesUseTheTraditionalAxisOrder)
    {
        EXPECT_FALSE(srsFromWkt(nullptr));
        EXPECT_FALSE(srsFromWkt(""));
        EXPECT_FALSE(srsFromWkt("not a wkt"));

        const SrsPtr wgs84 = srsFromEpsg(4326);
        ASSERT_TRUE(wgs84);
        EXPECT_EQ(OSRGetAxisMappingStrategy(wgs84.get()), OAMS_TRADITIONAL_GIS_ORDER);

        char *wkt = nullptr;
        ASSERT_EQ(OSRExportToWkt(wgs84.get(), &wkt), OGRERR_NONE);
        const std::string text = takeGdalString(wkt);
        const SrsPtr parsed = srsFromWkt(text.c_str());
        ASSERT_TRUE(parsed);
        EXPECT_TRUE(OSRIsSame(parsed.get(), wgs84.get()));
        EXPECT_EQ(takeGdalString(nullptr), "");
    }

    // validateAlignRaster opened both rasters, then returned without closing the reference
    // when the source could not be opened.
    TEST(Raii, Target_ValidateAlignClosesTheReferenceWhenTheSourceCannotBeOpened)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 64;
        spec.bands = 1;
        const fs::path reference = testutils::createGeoRaster(ta.getPath("reference.tif"), spec);

        const int before = openDatasetCount();
        const auto result =
            validateAlignRaster(ta.getPath("missing.tif").string(), reference.string());
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(openDatasetCount(), before);
    }
}
