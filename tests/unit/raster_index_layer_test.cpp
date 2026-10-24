/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-06: the vegetation-index map layer (renderRasterIndex: NDVI, NDRE, NDWI, SAVI, EVI on the
// bands 1..5) renders through the shared formula engine (VegetationEngine, vis_render.h), like
// the thumbnails and the tiles. Its own ramp (red, yellow, green over [-1, 1]) is the engine's
// colormap "ryg", which the engine does not list. Before, the layer had its own double-precision
// arithmetic and ramp, and looked only at the bands of its index for nodata: it still does
// (NodataMask::BandsRead), where every other caller of the engine leaves a pixel out when any band
// has none (NodataMask::AnyBand).

#include <memory>
#include <string>
#include <vector>

#include "json.h"

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

    // The plan of the layer: the fixed band mapping and its ramp
    FormulaPlan layerPlan(GDALDatasetH handle, const std::string &index)
    {
        FormulaPlan plan = planFormula(handle, index, VegetationEngine::parseFilter("RGBReN", 5), "", false);
        plan.colormap = VegetationEngine::instance().getColormap("ryg", true);
        return plan;
    }

    // What the engine renders over the raster itself with the plan of the layer: interleaved RGBA
    std::vector<uint8_t> renderEngine(const fs::path &raster, const std::string &index,
                                      NodataMask mask = NodataMask::BandsRead)
    {
        std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(raster.string().c_str(), GA_ReadOnly)));
        EXPECT_NE(ds, nullptr);
        GDALDatasetH handle = GDALDataset::ToHandle(ds.get());
        const RasterWindow window{0, 0, dq06::kSize, dq06::kSize, dq06::kSize, dq06::kSize};
        return renderPlannedFormulaRgba(handle, layerPlan(handle, index), window, "", mask);
    }

    std::string publicColormapIds()
    {
        std::string ids;
        for (const auto &colormap : VegetationEngine::instance().getColormapsJson())
            ids += colormap["id"].get<std::string>() + " ";
        return ids;
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
        const Colormap *ryg = VegetationEngine::instance().getColormap("ryg", true);
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

    // The ramp is the layer's, not a choice of the clients: not in the list, not a name a request
    // can use, and the layer renders with it all the same
    TEST_F(RasterIndexLayer, theRampIsNotAPublicColormap)
    {
        const VegetationEngine &engine = VegetationEngine::instance();
        EXPECT_EQ(publicColormapIds().find("ryg"), std::string::npos) << publicColormapIds();
        EXPECT_NE(publicColormapIds().find("rdylgn"), std::string::npos);
        EXPECT_EQ(engine.getColormap("ryg"), nullptr);
        EXPECT_NE(engine.getColormap("ryg", true), nullptr);
        for (const Colormap &colormap : engine.getColormaps())
            EXPECT_EQ(colormap.listed, colormap.id != "ryg") << colormap.id;

        TestArea ta(TEST_NAME, true);
        const fs::path ms = dq06::makeMultispectral(ta.getPath("ms.tif"));
        std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(ms.string().c_str(), GA_ReadOnly)));
        GDALDatasetH handle = GDALDataset::ToHandle(ds.get());
        EXPECT_THROW(planFormula(handle, "NDVI", VegetationEngine::parseFilter("RGBReN", 5), "ryg", true),
                     InvalidArgsException);

        // The layer draws with it: the pixels are red to green, never another colormap's
        const testutils::DecodedImage ndvi = renderLayer(ms, "NDVI");
        const Colormap *ryg = engine.getColormap("ryg", true);
        for (size_t p = 0; p < ndvi.bands[0].size(); p++)
        {
            if (ndvi.alpha()[p] == 0)
                continue;
            bool inRamp = false;
            for (const ColormapEntry &entry : ryg->entries)
                inRamp = inRamp || (entry.r == ndvi.bands[0][p] && entry.g == ndvi.bands[1][p] && entry.b == 0);
            ASSERT_TRUE(inRamp) << p;
        }
    }

    // The layer's nodata rule: a pixel is left out when a band its index READS has no value. The
    // patch of band 3 (blue) is read by EVI only, the patch of band 5 (NIR) by every index
    TEST_F(RasterIndexLayer, aPixelIsTransparentWhenABandTheIndexReadsHasNoValue)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path ms = dq06::makeMultispectral(ta.getPath("ms.tif"));
        for (const char *index : {"NDVI", "NDRE", "NDWI", "SAVI", "EVI"})
        {
            SCOPED_TRACE(index);
            const testutils::DecodedImage img = renderLayer(ms, index);
            ASSERT_TRUE(img.hasAlpha());
            const size_t width = static_cast<size_t>(img.width);
            const bool readsBlue = std::string(index) == "EVI";
            // Band 3 has nodata over x 20..39, y 20..39 (the raster's pixels)
            for (size_t y = 20; y < 40; y++)
                for (size_t x = 20; x < 40; x++)
                    EXPECT_EQ(img.alpha()[y * width + x], readsBlue ? 0 : 255) << x << "," << y;
            // Band 5 (NIR) has nodata over x 80..99, y 60..79
            for (size_t y = 60; y < 80; y++)
                for (size_t x = 80; x < 100; x++)
                    EXPECT_EQ(img.alpha()[y * width + x], 0) << x << "," << y;
            // A pixel with values in every band is opaque
            EXPECT_EQ(img.alpha()[5 * width + 5], 255);
        }
    }

    // The other rule is still the engine's default: where the layer draws the patch of band 3 for
    // NDVI, the engine's own render of the same formula leaves it out
    TEST_F(RasterIndexLayer, theEngineStillMasksByAnyBandUnlessAskedNotTo)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path ms = dq06::makeMultispectral(ta.getPath("ms.tif"));
        const std::vector<uint8_t> any = renderEngine(ms, "NDVI", NodataMask::AnyBand);
        const std::vector<uint8_t> read = renderEngine(ms, "NDVI", NodataMask::BandsRead);
        size_t patchDrawnOnlyByRead = 0;
        for (size_t p = 0; p < any.size() / 4; p++)
        {
            const size_t x = p % dq06::kSize, y = p / dq06::kSize;
            const bool inBlue = x >= 20 && x < 40 && y >= 20 && y < 40;
            if (inBlue)
            {
                EXPECT_EQ(any[p * 4 + 3], 0);
                patchDrawnOnlyByRead += read[p * 4 + 3] == 255;
            }
            else
            {
                // Everywhere else the two rules are the same pixels
                for (size_t c = 0; c < 4; c++)
                    ASSERT_EQ(any[p * 4 + c], read[p * 4 + c]) << x << "," << y;
            }
        }
        EXPECT_EQ(patchDrawnOnlyByRead, 400u);
    }

    // Alpha says "no data here" for every band: its 0 hides the pixel whatever the index reads
    TEST_F(RasterIndexLayer, aTransparentPixelStaysTransparent)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = dq06::kSize;
        spec.bands = 6;
        spec.type = GDT_Float32;
        spec.alphaBand = true;
        spec.epsg = 3857;
        spec.originX = dq06::kOriginX;
        spec.originY = dq06::kOriginY;
        spec.pixelSize = dq06::kPixel;
        spec.value = [](int band, int x, int y) { return dq06::reflectance(band, x, y); };
        const fs::path alphaRaster = ta.getPath("alpha.tif");
        testutils::createGeoRaster(alphaRaster, spec);
        {
            std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(alphaRaster.string().c_str(), GA_Update)));
            ASSERT_NE(ds, nullptr);
            std::vector<uint8_t> zeros(30 * 30, 0);
            ASSERT_EQ(ds->GetRasterBand(6)->RasterIO(GF_Write, 50, 50, 30, 30, zeros.data(), 30, 30, GDT_Byte, 0, 0),
                      CE_None);
        }
        for (const char *index : {"NDVI", "NDRE", "NDWI", "SAVI", "EVI"})
        {
            SCOPED_TRACE(index);
            const testutils::DecodedImage img = renderLayer(alphaRaster, index);
            const size_t width = static_cast<size_t>(img.width);
            for (size_t y = 50; y < 80; y++)
                for (size_t x = 50; x < 80; x++)
                    EXPECT_EQ(img.alpha()[y * width + x], 0) << x << "," << y;
            // Beside the hole the pixels have a value, and the pixels are those of the engine
            EXPECT_EQ(img.alpha()[5 * width + 5], 255);
            EXPECT_EQ(img.alpha()[49 * width + 49], 255);
            EXPECT_EQ(img.alpha()[80 * width + 80], 255);
        }
    }

    // BandsRead is only right if bandsRead is: a formula must not read a band its requiredBands
    // leave out, for every formula of the table that reads by the symbols of the filter (the two
    // thermal passthroughs read band 0 whatever the filter says, so they are not for BandsRead)
    TEST_F(RasterIndexLayer, everyFormulaReadsExactlyTheBandsItDeclares)
    {
        const VegetationEngine &engine = VegetationEngine::instance();
        BandFilter all;
        all.R = 0;
        all.G = 1;
        all.B = 2;
        all.N = 3;
        all.Re = 4;
        all.T = 5;
        constexpr size_t kPixels = 64;
        std::vector<std::vector<float>> bands(6, std::vector<float>(kPixels));
        for (int b = 0; b < 6; b++)
            for (size_t i = 0; i < kPixels; i++)
                bands[b][i] = 0.1f + static_cast<float>((i * (3 + b) + 7 * b) % 53) / 60.0f;

        for (const VegetationFormula &formula : engine.getAllFormulas())
        {
            SCOPED_TRACE(formula.id);
            if (formula.requiredBands == "T")
                continue;
            const std::vector<int> read = VegetationEngine::bandsRead(formula, all);
            EXPECT_FALSE(read.empty());
            std::vector<float *> full, onlyRead(6, nullptr);
            for (int b = 0; b < 6; b++)
                full.push_back(bands[b].data());
            for (const int b : read)
                onlyRead[b] = bands[b].data();
            std::vector<float> expected(kPixels), actual(kPixels);
            engine.applyFormula(formula, all, full, expected.data(), kPixels, NODATA_SENTINEL);
            engine.applyFormula(formula, all, onlyRead, actual.data(), kPixels, NODATA_SENTINEL);
            EXPECT_EQ(expected, actual);
        }

        // The bands of the five indices of the layer, on the bands 1..5 (R G B Re N)
        const BandFilter layer = VegetationEngine::parseFilter("RGBReN", 5);
        EXPECT_EQ(VegetationEngine::bandsRead(*engine.getFormula("NDVI"), layer), (std::vector<int>{0, 4}));
        EXPECT_EQ(VegetationEngine::bandsRead(*engine.getFormula("NDRE"), layer), (std::vector<int>{3, 4}));
        EXPECT_EQ(VegetationEngine::bandsRead(*engine.getFormula("NDWI"), layer), (std::vector<int>{1, 4}));
        EXPECT_EQ(VegetationEngine::bandsRead(*engine.getFormula("SAVI"), layer), (std::vector<int>{0, 4}));
        EXPECT_EQ(VegetationEngine::bandsRead(*engine.getFormula("EVI"), layer), (std::vector<int>{0, 2, 4}));
    }
}
