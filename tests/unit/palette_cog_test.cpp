/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The COG of a raster with a colour table keeps the table as GDAL's tools kept it: gdalwarp gives
// the first band of its output the colour table of the source, and GTiff writes it for an image of
// 1 or 2 bands, the second one alpha. That holds for a palette band beside an alpha band (a TIFF of
// 2 samples), and for a palette band with an internal mask, which the warp turns into an alpha band
// (-dstalpha, as DroneDB 2.5 asked gdalwarp).

#include <array>
#include <string>
#include <vector>

#include "cog.h"
#include "ddb.h"
#include "gdal_raii.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"

namespace {
using namespace ddb;

constexpr int kWidth = 700, kHeight = 500;

// The colour table of the sources: 40 colours, the others black
std::vector<std::array<short, 4>> sourceColours() {
    std::vector<std::array<short, 4>> colours;
    for (int i = 0; i < 40; i++)
        colours.push_back({static_cast<short>((i * 61) % 256),
                           static_cast<short>((255 - i * 37) % 256),
                           static_cast<short>((i * i * 7) % 256),
                           255});
    return colours;
}

// A raster of palette indices in `epsg`, with an alpha band, an internal mask or neither, written
// by `driver` (GTiff, or COG for one that is already optimized)
fs::path writePalette(const fs::path& path, int epsg, bool alpha, bool mask, const char* driver) {
    const fs::path plain = path.parent_path() / ("plain-" + path.filename().string());
    {
        auto ds = adoptDataset(GDALCreate(GDALGetDriverByName("GTiff"),
                                          io::toUtf8(plain).c_str(),
                                          kWidth,
                                          kHeight,
                                          alpha ? 2 : 1,
                                          GDT_Byte,
                                          nullptr));
        EXPECT_TRUE(ds);
        GDALDatasetH h = datasetHandle(ds);
        const double gt[6] = {epsg == 3857 ? 1113000.0 : 500000.0,
                              4,
                              0,
                              epsg == 3857 ? 5621000.0 : 5000000.0,
                              0,
                              -4};
        GDALSetGeoTransform(h, const_cast<double*>(gt));
        SrsPtr srs(OSRNewSpatialReference(nullptr));
        OSRImportFromEPSG(srs.get(), epsg);
        GDALSetSpatialRef(h, srs.get());
        GDALColorTableH table = GDALCreateColorTable(GPI_RGB);
        const auto colours = sourceColours();
        for (size_t i = 0; i < colours.size(); i++) {
            const GDALColorEntry e{colours[i][0], colours[i][1], colours[i][2], colours[i][3]};
            GDALSetColorEntry(table, static_cast<int>(i), &e);
        }
        GDALSetRasterColorTable(GDALGetRasterBand(h, 1), table);
        GDALDestroyColorTable(table);
        std::vector<uint8_t> plane(static_cast<size_t>(kWidth) * kHeight);
        for (int b = 1; b <= (alpha ? 2 : 1); b++) {
            for (int y = 0; y < kHeight; y++)
                for (int x = 0; x < kWidth; x++)
                    plane[static_cast<size_t>(y) * kWidth + x] =
                        b == 1 ? static_cast<uint8_t>((x / 9 + y / 7) % 40)
                               : ((x / 11 + y / 13) % 5 == 0 ? 0 : 255);
            if (b == 2)
                GDALSetRasterColorInterpretation(GDALGetRasterBand(h, b), GCI_AlphaBand);
            EXPECT_EQ(GDALRasterIO(GDALGetRasterBand(h, b),
                                   GF_Write,
                                   0,
                                   0,
                                   kWidth,
                                   kHeight,
                                   plane.data(),
                                   kWidth,
                                   kHeight,
                                   GDT_Byte,
                                   0,
                                   0),
                      CE_None);
        }
        if (mask) {
            EXPECT_EQ(GDALCreateDatasetMaskBand(h, GMF_PER_DATASET), CE_None);
            for (int y = 0; y < kHeight; y++)
                for (int x = 0; x < kWidth; x++)
                    plane[static_cast<size_t>(y) * kWidth + x] = (x + y) % 13 < 3 ? 0 : 255;
            EXPECT_EQ(GDALRasterIO(GDALGetMaskBand(GDALGetRasterBand(h, 1)),
                                   GF_Write,
                                   0,
                                   0,
                                   kWidth,
                                   kHeight,
                                   plane.data(),
                                   kWidth,
                                   kHeight,
                                   GDT_Byte,
                                   0,
                                   0),
                      CE_None);
        }
    }
    if (std::string(driver) == "GTiff") {
        fs::rename(plain, path);
        return path;
    }
    const auto src = testutils::openRaster(plain);
    auto cog = adoptDataset(GDALCreateCopy(GDALGetDriverByName(driver),
                                           io::toUtf8(path).c_str(),
                                           datasetHandle(src),
                                           FALSE,
                                           nullptr,
                                           nullptr,
                                           nullptr));
    EXPECT_TRUE(cog);
    return path;
}

// The colour table GDAL reads of band 1, entry by entry
std::vector<std::array<short, 4>> colourTableOf(GDALRasterBandH band) {
    std::vector<std::array<short, 4>> colours;
    if (GDALColorTableH table = GDALGetRasterColorTable(band))
        for (int i = 0; i < GDALGetColorEntryCount(table); i++) {
            const GDALColorEntry* e = GDALGetColorEntry(table, i);
            colours.push_back({e->c1, e->c2, e->c3, e->c4});
        }
    return colours;
}

TEST(PaletteCog, theCogOfAPaletteRasterHasItsColourTableBesideAnAlphaBand) {
    DDBRegisterProcess(false);
    TestArea ta(TEST_NAME, true);
    struct Case {
        std::string name;
        int epsg;
        bool alpha, mask;
        const char* driver;
    };
    // A warp (a raster that is not an optimized COG) keeps a palette band alone, keeps the alpha
    // band beside it, and turns an internal mask into an alpha band; an optimized COG is encoded
    // again at its own grid with its alpha band
    for (const Case& c : std::vector<Case>{{"warped", 32633, false, false, "GTiff"},
                                           {"warped-alpha", 32633, true, false, "GTiff"},
                                           {"warped-mask", 32633, false, true, "GTiff"},
                                           {"own-grid-alpha", 3857, true, false, "COG"}}) {
        SCOPED_TRACE(c.name);
        const fs::path input =
            writePalette(ta.getPath(c.name + ".tif"), c.epsg, c.alpha, c.mask, c.driver);
        const fs::path output = ta.getPath(c.name + "-cog.tif");
        ASSERT_NO_THROW(buildCog(io::toUtf8(input), io::toUtf8(output)));

        const auto src = testutils::openRaster(input);
        const auto cog = testutils::openRaster(output);
        ASSERT_TRUE(src && cog);
        const int bands = c.alpha || c.mask ? 2 : 1;
        ASSERT_EQ(GDALGetRasterCount(datasetHandle(cog)), bands);
        GDALRasterBandH first = GDALGetRasterBand(datasetHandle(cog), 1);
        EXPECT_EQ(GDALGetRasterColorInterpretation(first), GCI_PaletteIndex);
        if (bands == 2) {
            EXPECT_EQ(GDALGetRasterColorInterpretation(GDALGetRasterBand(datasetHandle(cog), 2)),
                      GCI_AlphaBand);
        }
        const auto want = colourTableOf(GDALGetRasterBand(datasetHandle(src), 1));
        ASSERT_EQ(want.size(), 256u) << "the source's table, as GTiff reads it back";
        EXPECT_EQ(colourTableOf(first), want);
        if (c.epsg == 3857) {
            // At its own grid the COG has the pixels of its source
            for (int b = 1; b <= 2; b++) {
                std::vector<uint8_t> a(static_cast<size_t>(kWidth) * kHeight), z(a.size());
                ASSERT_EQ(GDALRasterIO(GDALGetRasterBand(datasetHandle(src), b),
                                       GF_Read,
                                       0,
                                       0,
                                       kWidth,
                                       kHeight,
                                       a.data(),
                                       kWidth,
                                       kHeight,
                                       GDT_Byte,
                                       0,
                                       0),
                          CE_None);
                ASSERT_EQ(GDALRasterIO(GDALGetRasterBand(datasetHandle(cog), b),
                                       GF_Read,
                                       0,
                                       0,
                                       kWidth,
                                       kHeight,
                                       z.data(),
                                       kWidth,
                                       kHeight,
                                       GDT_Byte,
                                       0,
                                       0),
                          CE_None);
                EXPECT_EQ(a, z) << "band " << b;
            }
        }
    }
}
}  // namespace
