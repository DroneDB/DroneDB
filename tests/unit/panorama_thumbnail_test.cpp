/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Panoramas (JPEG/TIFF images twice as wide as high) have a thumbnail of their own, rendered from
// the file like the one of an Image. libddb 2.0.0 reported thumbnail "none" for Panorama and
// GeoPanorama in DDBGetEntryTypes, and Registry, which follows that field, stopped rendering them.
// Video stays "none".

#include <cstdint>
#include <string>

#include "ddb.h"
#include "entry_types.h"
#include "exifeditor.h"
#include "fs.h"
#include "gdal_inc.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "thumbs.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

    std::string take(char *s)
    {
        std::string copy = s != nullptr ? s : "";
        DDBFree(s);
        return copy;
    }

    // A JPEG of width x height pixels (GDAL), without EXIF
    void writeJpeg(const fs::path &path, int width, int height)
    {
        GDALDatasetH mem = GDALCreate(GDALGetDriverByName("MEM"), "", width, height, 3, GDT_Byte, nullptr);
        ASSERT_NE(mem, nullptr);
        for (int b = 1; b <= 3; b++)
            ASSERT_EQ(GDALFillRaster(GDALGetRasterBand(mem, b), 40.0 * b, 0.0), CE_None);
        GDALDatasetH jpeg = GDALCreateCopy(GDALGetDriverByName("JPEG"), io::toUtf8(path).c_str(), mem, FALSE,
                                           nullptr, nullptr, nullptr);
        ASSERT_NE(jpeg, nullptr) << CPLGetLastErrorMsg();
        GDALClose(jpeg);
        GDALClose(mem);
    }

    std::string thumbnailOf(const json &types, const std::string &name)
    {
        for (const auto &t : types)
            if (t["name"] == name)
                return t["thumbnail"];
        return "<missing>";
    }

    TEST(PanoramaThumbnail, entryTypesReportTheFileAsTheThumbnailSource)
    {
        char *out = nullptr;
        ASSERT_DDB_OK(DDBGetEntryTypes(&out));
        const json types = json::parse(take(out));
        EXPECT_EQ(thumbnailOf(types, "Image"), "file");
        EXPECT_EQ(thumbnailOf(types, "GeoImage"), "file");
        EXPECT_EQ(thumbnailOf(types, "Panorama"), "file");
        EXPECT_EQ(thumbnailOf(types, "GeoPanorama"), "file");
        EXPECT_EQ(thumbnailOf(types, "Video"), "none");
        EXPECT_EQ(thumbnailOf(types, "GeoVideo"), "none");
    }

    // The thumbnail generators render a Panorama and a GeoPanorama through the C API
    TEST(PanoramaThumbnail, panoramasGetAThumbnailThroughTheCApi)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path folder = ta.getFolder("ds");
        writeJpeg(folder / "pano.jpg", 128, 64);
        writeJpeg(folder / "geopano.jpg", 128, 64);
        const std::string folderUtf8 = io::toUtf8(folder);
        const std::string pano = io::toUtf8(folder / "pano.jpg");
        const std::string geopano = io::toUtf8(folder / "geopano.jpg");
        ExifEditor(geopano).SetGPS(46.0667, 11.1167, 194.0);

        char *out = nullptr;
        ASSERT_DDB_OK(DDBInit(folderUtf8.c_str(), &out));
        take(out);
        const char *paths[] = {pano.c_str(), geopano.c_str()};
        ASSERT_DDB_OK(DDBAdd(folderUtf8.c_str(), paths, 2, &out, false));
        const json added = json::parse(take(out));
        ASSERT_EQ(added.size(), 2u) << added.dump();
        for (const auto &e : added)
            EXPECT_EQ(e["type"], static_cast<int>(e["path"] == "pano.jpg" ? EntryType::Panorama : EntryType::GeoPanorama))
                << e.dump();

        for (const auto &input : {pano, geopano})
        {
            SCOPED_TRACE(input);
            // To a file
            const fs::path thumb = ta.getPath(fs::path(input).filename().replace_extension(".webp"));
            ASSERT_DDB_OK(DDBGenerateThumbnail(input.c_str(), 64, io::toUtf8(thumb).c_str()));
            EXPECT_GT(fs::file_size(thumb), 0u);
            // To memory
            uint8_t *buffer = nullptr;
            int size = 0;
            ASSERT_DDB_OK(DDBGenerateMemoryThumbnail(input.c_str(), 64, &buffer, &size));
            ASSERT_GT(size, 12);
            EXPECT_EQ(std::string(reinterpret_cast<char *>(buffer), 4), "RIFF");
            EXPECT_EQ(std::string(reinterpret_cast<char *>(buffer) + 8, 4), "WEBP");
            DDBVSIFree(buffer);
            // The thumbnails command (ddb thumbs) skips what supportsThumbnails() refuses
            const fs::path cliThumb = ta.getPath("cli-" + fs::path(input).filename().replace_extension(".webp").string());
            generateThumbs({input}, cliThumb, 64, false);
            EXPECT_TRUE(fs::exists(cliThumb)) << "generateThumbs skipped a panorama";
        }
    }
} // namespace
