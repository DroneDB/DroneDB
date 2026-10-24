/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-06: the vegetation-index map layer (renderRasterIndex: NDVI, NDRE, NDWI, SAVI, EVI on the
// bands 1..5) renders through the shared formula engine (VegetationEngine, vis_render.h), like
// the thumbnails and the tiles. Its own ramp (red, yellow, green over [-1, 1]) is the engine's
// colormap "ryg". Before, the layer had its own double-precision arithmetic and ramp, and looked
// only at the bands of its index for nodata; the engine leaves a pixel out when any band has none.

#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "raster_test_utils.h"

#include "ddb.h"
#include "dq06_raster_support.h"
#include "raster_region.h"
#include "raster_utils.h"
#include "vegetation.h"
#include "vis_render.h"

namespace
{
    using namespace ddb;

    class RasterIndexLayer : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    // The extent of the raster, so the layer's warp is an identity
    const double kBbox[4] = {dq06::kOriginX, dq06::kOriginY - dq06::kSize * dq06::kPixel,
                             dq06::kOriginX + dq06::kSize * dq06::kPixel, dq06::kOriginY};

    testutils::DecodedImage renderLayer(const fs::path &raster, const std::string &index)
    {
        uint8_t *buffer = nullptr;
        int size = 0;
        renderRasterIndex(raster.string(), index, kBbox, "EPSG:3857", dq06::kSize, dq06::kSize, "image/png", &buffer,
                          &size);
        return testutils::decodeImage(testutils::takeBuffer(buffer, size));
    }

    // What the engine renders over the raster itself with the colormap and the fixed band
    // mapping of the layer: interleaved RGBA
    std::vector<uint8_t> renderEngine(const fs::path &raster, const std::string &index)
    {
        std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(raster.string().c_str(), GA_ReadOnly)));
        EXPECT_NE(ds, nullptr);
        GDALDatasetH handle = GDALDataset::ToHandle(ds.get());
        const FormulaPlan plan = planFormula(handle, index, VegetationEngine::parseFilter("RGBReN", 5), "ryg", true);
        const RasterWindow window{0, 0, dq06::kSize, dq06::kSize, dq06::kSize, dq06::kSize};
        return renderPlannedFormulaRgba(handle, plan, window, "");
    }

    // Number of pixels of img that are not those of the interleaved rgba
    size_t differences(const testutils::DecodedImage &img, const std::vector<uint8_t> &rgba)
    {
        EXPECT_EQ(img.bands.size(), 4u);
        size_t different = 0;
        for (size_t p = 0; p < rgba.size() / 4; p++)
        {
            for (size_t c = 0; c < 4; c++)
            {
                if (img.bands[c][p] != rgba[p * 4 + c])
                {
                    different++;
                    break;
                }
            }
        }
        return different;
    }

    // The ramp the layer has always had, in the engine's table: pure red, yellow, green
    TEST_F(RasterIndexLayer, theLayersRampIsAColormapOfTheEngine)
    {
        const Colormap *ryg = VegetationEngine::instance().getColormap("ryg");
        ASSERT_NE(ryg, nullptr);
        EXPECT_EQ(ryg->entries[0].r, 255);
        EXPECT_EQ(ryg->entries[0].g, 0);
        EXPECT_EQ(ryg->entries[0].b, 0);
        EXPECT_EQ(ryg->entries[255].r, 0);
        EXPECT_EQ(ryg->entries[255].g, 255);
        EXPECT_EQ(ryg->entries[255].b, 0);
        // Yellow in the middle, red above and green below it
        EXPECT_GE(ryg->entries[127].r, 254);
        EXPECT_GE(ryg->entries[127].g, 253);
        EXPECT_GE(ryg->entries[128].g, 254);
        EXPECT_LE(ryg->entries[128].r, 254);
        for (const ColormapEntry &entry : ryg->entries)
        {
            EXPECT_EQ(entry.b, 0);
            EXPECT_EQ(entry.a, 255);
        }
    }

    // One implementation: every pixel of the layer is the pixel the engine renders
    TEST_F(RasterIndexLayer, everyIndexIsWhatTheEngineRenders)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path ms = dq06::makeMultispectral(ta.getPath("ms.tif"));
        for (const char *index : {"NDVI", "NDRE", "NDWI", "SAVI", "EVI"})
        {
            SCOPED_TRACE(index);
            EXPECT_EQ(differences(renderLayer(ms, index), renderEngine(ms, index)), 0u);
        }
    }

    // The names are case-insensitive, and the engine's other formulas are not index layers
    TEST_F(RasterIndexLayer, namesAreCaseInsensitiveAndOnlyTheFiveIndicesRender)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path ms = dq06::makeMultispectral(ta.getPath("ms.tif"));
        EXPECT_EQ(differences(renderLayer(ms, "ndvi"), renderEngine(ms, "NDVI")), 0u);
        EXPECT_EQ(differences(renderLayer(ms, "Savi"), renderEngine(ms, "SAVI")), 0u);
        uint8_t *buffer = nullptr;
        int size = 0;
        for (const char *name : {"GLI", "VARI", "NOPE", ""})
            EXPECT_THROW(renderRasterIndex(ms.string(), name, kBbox, "EPSG:3857", 16, 16, "image/png", &buffer, &size),
                         InvalidArgsException)
                << name;
    }

    // The engine's nodata rule: a pixel with no value in any band is transparent, also when the
    // index does not read that band (the patch of band 3 is not read by NDVI)
    TEST_F(RasterIndexLayer, aPixelWithNodataInAnyBandIsTransparent)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path ms = dq06::makeMultispectral(ta.getPath("ms.tif"));
        const testutils::DecodedImage ndvi = renderLayer(ms, "NDVI");
        ASSERT_TRUE(ndvi.hasAlpha());
        const size_t width = static_cast<size_t>(ndvi.width);
        // Band 3 has nodata over x 20..39, y 20..39 (the raster's pixels)
        for (size_t y = 20; y < 40; y++)
            for (size_t x = 20; x < 40; x++)
                EXPECT_EQ(ndvi.alpha()[y * width + x], 0) << x << "," << y;
        // And a pixel with values in every band is opaque
        EXPECT_EQ(ndvi.alpha()[5 * width + 5], 255);
    }
}
