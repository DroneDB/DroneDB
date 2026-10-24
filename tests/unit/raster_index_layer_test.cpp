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

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
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
#include "gdaltiler.h"
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
        renderRasterIndex(testutils::regionRequest(raster.string(), kBbox, "EPSG:3857", dq06::kSize, dq06::kSize, "image/png"), index, &buffer, &size);
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

    // The multispectral raster of the DQ-06 tests with a sixth band that is alpha, 255 everywhere: a
    // Float32 raster, or a UInt16 one (the bands are scaled to its range, as ODM writes them)
    fs::path makeMultispectralWithAlpha(const fs::path &path, GDALDataType type, bool withPatches)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = dq06::kSize;
        spec.bands = 6;
        spec.type = type;
        spec.alphaBand = true;
        spec.epsg = 3857;
        spec.originX = dq06::kOriginX;
        spec.originY = dq06::kOriginY;
        spec.pixelSize = dq06::kPixel;
        // The nodata is -9999, or 0 where the type cannot hold a negative value
        const double scale = type == GDT_UInt16 ? 60000.0 : 1.0;
        const double nodata = type == GDT_UInt16 ? 0.0 : dq06::kNodata;
        spec.value = [=](int band, int x, int y) {
            const double value = withPatches ? dq06::reflectanceWithNodata(band, x, y, nodata / scale)
                                             : dq06::reflectance(band, x, y);
            return std::floor(value * scale * 1000.0) / 1000.0;
        };
        testutils::createGeoRaster(path, spec);
        if (withPatches)
            dq06::setNodata(path, nodata);
        return path;
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
            EXPECT_THROW(renderRasterIndex(testutils::regionRequest(ms.string(), kBbox, "EPSG:3857", 16, 16, "image/png"), name, &buffer, &size),
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
        const fs::path alphaRaster = makeMultispectralWithAlpha(ta.getPath("alpha.tif"), GDT_Float32, false);
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

    // ODM writes the alpha band of a UInt16 orthophoto as 255. The warp of the layer must not rescale
    // it by the 65535 of the type, which made every pixel of such a raster transparent
    TEST_F(RasterIndexLayer, anAlphaOf255InAUInt16BandIsOpaque)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path raster = makeMultispectralWithAlpha(ta.getPath("odm.tif"), GDT_UInt16, false);
        // SAVI and EVI are not drawn over integer data (they are refused, see below)
        for (const char *index : {"NDVI", "NDRE", "NDWI"})
        {
            SCOPED_TRACE(index);
            const testutils::DecodedImage img = renderLayer(raster, index);
            size_t transparent = 0;
            for (const uint8_t alpha : img.alpha())
                transparent += alpha == 0;
            EXPECT_EQ(transparent, 0u);
        }
    }

    // A raster with an alpha band and a nodata value: the nodata pixels of a band are nodata (they
    // were turned into valid zeros when the warp took the alpha for the validity of the pixels)
    TEST_F(RasterIndexLayer, theNodataOfARasterWithAlphaIsNodataOfItsBands)
    {
        TestArea ta(TEST_NAME, true);
        for (const GDALDataType type : {GDT_Float32, GDT_UInt16})
        {
            const fs::path raster = makeMultispectralWithAlpha(
                ta.getPath(type == GDT_Float32 ? "f32.tif" : "u16.tif"), type, true);
            const testutils::DecodedImage ndvi = renderLayer(raster, "NDVI");
            const size_t width = static_cast<size_t>(ndvi.width);
            SCOPED_TRACE(type == GDT_Float32 ? "Float32" : "UInt16");
            // Band 3 (blue) is not read by NDVI: its patch is drawn
            for (size_t y = 20; y < 40; y++)
                for (size_t x = 20; x < 40; x++)
                    EXPECT_EQ(ndvi.alpha()[y * width + x], 255) << x << "," << y;
            // Band 5 (NIR) is: its patch is not
            for (size_t y = 60; y < 80; y++)
                for (size_t x = 80; x < 100; x++)
                    EXPECT_EQ(ndvi.alpha()[y * width + x], 0) << x << "," << y;
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

    // ---- helpers of the tests below ---------------------------------------------------------

    // The colour the layer's ramp (pure red, yellow, green over [-1, 1], 256 steps) gives to a value,
    // written out here from the ramp's three stops and not read from the engine: the index is
    // normalised to [0, 1] and cut into one of 256 steps, then red stays 255 until the middle and
    // falls to 0, green rises from 0 to 255 up to the middle and stays there
    std::array<int, 3> rampColor(double value)
    {
        const double normalised = std::clamp((value + 1.0) / 2.0, 0.0, 1.0);
        const int step = static_cast<int>(normalised * 255.0);
        const double t = step / 255.0;
        const double red = t < 0.5 ? 255.0 : 255.0 * (1.0 - (t - 0.5) / 0.5);
        const double green = t < 0.5 ? 255.0 * (t / 0.5) : 255.0;
        return {static_cast<int>(red), static_cast<int>(green), 0};
    }

    // The pixel of an image: R, G, B, A
    std::array<int, 4> pixelOf(const testutils::DecodedImage &img, int x, int y)
    {
        const size_t p = static_cast<size_t>(y) * img.width + x;
        return {img.bands[0][p], img.bands[1][p], img.bands[2][p], img.bands[3][p]};
    }

    // The colour of a pixel is that of `value` in the ramp. A value on the border of two of the 256
    // steps may fall in either (the engine computes it in float), and a step is 2 levels of green or red
    void expectRamp(const testutils::DecodedImage &img, int x, int y, double value, int alpha = 255)
    {
        const auto want = rampColor(value);
        const auto got = pixelOf(img, x, y);
        EXPECT_NEAR(got[0], want[0], 2) << "R of " << x << "," << y << " for the value " << value;
        EXPECT_NEAR(got[1], want[1], 2) << "G of " << x << "," << y << " for the value " << value;
        EXPECT_EQ(got[2], 0);
        EXPECT_EQ(got[3], alpha) << "alpha of " << x << "," << y;
    }

    // A raster of constant bands, each band named: the layer's tests of the band order. `alphaValue`
    // adds a last band that is alpha.
    struct ConstantRaster
    {
        GDALDataType type = GDT_Float32;
        std::vector<std::string> names;
        std::vector<double> values;
        bool alpha = false;
        std::function<double(int, int)> alphaValue = [](int, int) { return 255.0; };
        // A band is `fill` where the alpha is 0 (the bands of an orthophoto are 0 outside of it)
        double fill = 0.0;
        int size = dq06::kSize;
    };

    fs::path makeConstantRaster(const fs::path &path, const ConstantRaster &raster)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = raster.size;
        spec.bands = static_cast<int>(raster.values.size()) + (raster.alpha ? 1 : 0);
        spec.type = raster.type;
        spec.alphaBand = raster.alpha;
        spec.alphaValue = raster.alphaValue;
        spec.bandNames = raster.names;
        spec.epsg = 3857;
        spec.originX = dq06::kOriginX;
        spec.originY = dq06::kOriginY;
        spec.pixelSize = dq06::kPixel;
        spec.value = [raster](int band, int x, int y) {
            if (raster.alpha && raster.alphaValue(x, y) == 0.0)
                return raster.fill;
            return raster.values[band - 1];
        };
        return testutils::createGeoRaster(path, spec);
    }

    testutils::DecodedImage renderWindow(const fs::path &raster, const std::string &index, const double bbox[4],
                                         const std::string &format = "image/png")
    {
        uint8_t *buffer = nullptr;
        int size = 0;
        renderRasterIndex(testutils::regionRequest(raster.string(), bbox, "EPSG:3857", dq06::kSize, dq06::kSize, format), index, &buffer, &size);
        return testutils::decodeImage(testutils::takeBuffer(buffer, size));
    }

    std::string expectInvalidArgs(const fs::path &raster, const std::string &index)
    {
        uint8_t *buffer = nullptr;
        int size = 0;
        try
        {
            renderRasterIndex(testutils::regionRequest(raster.string(), kBbox, "EPSG:3857", 16, 16, "image/png"), index, &buffer, &size);
        }
        catch (const InvalidArgsException &e)
        {
            return e.what();
        }
        ADD_FAILURE() << index << " was rendered";
        VSIFree(buffer);
        return "";
    }

    // The C API's raster info of a raster: what the Registry reads to advertise the styles
    json rasterInfo(const fs::path &raster)
    {
        char *buffer = nullptr;
        EXPECT_EQ(DDBGetRasterInfo(raster.string().c_str(), &buffer), DDBERR_NONE) << DDBGetLastError();
        json info = json::parse(buffer);
        DDBFree(buffer);
        return info;
    }

    std::vector<std::string> indexLayerOf(const fs::path &raster)
    {
        const json info = rasterInfo(raster);
        EXPECT_TRUE(info.contains("indexLayer")) << info.dump();
        return info.value("indexLayer", json::array()).get<std::vector<std::string>>();
    }

    const std::vector<std::string> kAllFive = {"NDVI", "NDRE", "NDWI", "SAVI", "EVI"};

    // ---- R1: the bands of the layer come from the sensor profile ------------------------------------

    // MicaSense RedEdge, Altum, RedEdge-P and the DJI P4 Multispectral write Blue, Green, Red, Red
    // edge, NIR. The layer read band 3 as blue and band 1 as red there
    TEST_F(RasterIndexLayer, theBandsAreReadByTheirNameWhenTheRasterNamesThem)
    {
        TestArea ta(TEST_NAME, true);
        ConstantRaster raster;
        raster.names = {"Blue", "Green", "Red", "Red edge", "NIR"};
        raster.values = {0.05, 0.10, 0.20, 0.30, 0.60};
        const fs::path micasense = makeConstantRaster(ta.getPath("micasense.tif"), raster);

        const double kBboxArray[4] = {kBbox[0], kBbox[1], kBbox[2], kBbox[3]};
        // NDVI = (N - R) / (N + R) = 0.4 / 0.8 = 0.5 (it was (0.6 - 0.05) / 0.65 = 0.846 with R = band 1)
        expectRamp(renderWindow(micasense, "NDVI", kBboxArray), 64, 64, 0.5);
        // NDRE = (N - Re) / (N + Re) = 0.3 / 0.9; NDWI = (G - N) / (G + N) = -0.5 / 0.7
        expectRamp(renderWindow(micasense, "NDRE", kBboxArray), 64, 64, 1.0 / 3.0);
        expectRamp(renderWindow(micasense, "NDWI", kBboxArray), 64, 64, -5.0 / 7.0);
        // SAVI = 1.5 (N - R) / (N + R + 0.5) = 0.6 / 1.3 (it was 1.5 * 0.55 / 1.15 with R = band 1)
        expectRamp(renderWindow(micasense, "SAVI", kBboxArray), 64, 64, 0.6 / 1.3);
        // EVI = 2.5 (N - R) / (N + 6R - 7.5B + 1) = 1.0 / (0.6 + 1.2 - 0.375 + 1) = 1 / 2.425
        expectRamp(renderWindow(micasense, "EVI", kBboxArray), 64, 64, 1.0 / 2.425);

        EXPECT_EQ(indexLayerOf(micasense), kAllFive);
    }

    // Mavic 3 Multispectral and Sequoia: Green, Red, Red edge, NIR. No blue: no EVI
    TEST_F(RasterIndexLayer, aFourBandRasterRendersTheIndicesItsBandsAllow)
    {
        TestArea ta(TEST_NAME, true);
        ConstantRaster raster;
        raster.names = {"Green", "Red", "Red edge", "NIR"};
        raster.values = {0.10, 0.20, 0.30, 0.60};
        const fs::path mavic = makeConstantRaster(ta.getPath("mavic.tif"), raster);
        const double bbox[4] = {kBbox[0], kBbox[1], kBbox[2], kBbox[3]};

        expectRamp(renderWindow(mavic, "NDVI", bbox), 64, 64, 0.5);
        expectRamp(renderWindow(mavic, "NDRE", bbox), 64, 64, 1.0 / 3.0);
        expectRamp(renderWindow(mavic, "NDWI", bbox), 64, 64, -5.0 / 7.0);
        expectRamp(renderWindow(mavic, "SAVI", bbox), 64, 64, 0.6 / 1.3);
        const std::string refusal = expectInvalidArgs(mavic, "EVI");
        EXPECT_NE(refusal.find("Blue"), std::string::npos) << refusal;
        EXPECT_EQ(indexLayerOf(mavic), (std::vector<std::string>{"NDVI", "NDRE", "NDWI", "SAVI"}));
    }

    // ODM names the bands of a multispectral orthophoto as the camera lists them (Red, Green, NIR,
    // Rededge here) and adds an alpha band of 255 to the UInt16 raster
    TEST_F(RasterIndexLayer, anOdmRasterWithItsOwnBandOrderRendersTheRatioIndices)
    {
        TestArea ta(TEST_NAME, true);
        ConstantRaster raster;
        raster.type = GDT_UInt16;
        raster.names = {"Red", "Green", "NIR", "Rededge"};
        raster.values = {12000, 20000, 36000, 24000};
        raster.alpha = true;
        const fs::path odm = makeConstantRaster(ta.getPath("odm.tif"), raster);
        const double bbox[4] = {kBbox[0], kBbox[1], kBbox[2], kBbox[3]};

        // (N - R) / (N + R) = 24000 / 48000; (N - Re) / (N + Re) = 12000 / 60000; (G - N) / (G + N) = -16000 / 56000
        expectRamp(renderWindow(odm, "NDVI", bbox), 64, 64, 0.5);
        expectRamp(renderWindow(odm, "NDRE", bbox), 64, 64, 0.2);
        expectRamp(renderWindow(odm, "NDWI", bbox), 64, 64, -2.0 / 7.0);
        EXPECT_EQ(indexLayerOf(odm), (std::vector<std::string>{"NDVI", "NDRE", "NDWI"}));
    }

    // A raster whose bands the sensor profiles do not name keeps the order the layer always had
    // (R, G, B, Red edge, NIR): nothing that rendered stops rendering
    TEST_F(RasterIndexLayer, anUnnamedFiveBandRasterKeepsTheLegacyOrder)
    {
        TestArea ta(TEST_NAME, true);
        ConstantRaster raster;
        raster.values = {0.20, 0.10, 0.05, 0.30, 0.60}; // R G B Re N
        const fs::path unnamed = makeConstantRaster(ta.getPath("unnamed.tif"), raster);
        const double bbox[4] = {kBbox[0], kBbox[1], kBbox[2], kBbox[3]};

        expectRamp(renderWindow(unnamed, "NDVI", bbox), 64, 64, 0.5);
        expectRamp(renderWindow(unnamed, "EVI", bbox), 64, 64, 1.0 / 2.425);
        EXPECT_EQ(indexLayerOf(unnamed), kAllFive);

        // The same without names but in 16 bits: the ratio indices
        raster.type = GDT_UInt16;
        raster.values = {12000, 6000, 3000, 18000, 36000};
        const fs::path unnamed16 = makeConstantRaster(ta.getPath("unnamed16.tif"), raster);
        expectRamp(renderWindow(unnamed16, "NDVI", bbox), 64, 64, 0.5);
        EXPECT_EQ(indexLayerOf(unnamed16), (std::vector<std::string>{"NDVI", "NDRE", "NDWI"}));

        // The synthetic raster of the DQ-06 tests
        EXPECT_EQ(indexLayerOf(dq06::makeMultispectral(ta.getPath("ms.tif"))), kAllFive);
    }

    // Neither the names nor the band count give the bands: nothing to draw, and the reason says which bands
    TEST_F(RasterIndexLayer, aRasterWithoutTheBandsOfAnIndexRefusesItNamingTheMissingBands)
    {
        TestArea ta(TEST_NAME, true);
        ConstantRaster raster;
        raster.type = GDT_Byte;
        raster.values = {100, 150, 50};
        const fs::path rgb = makeConstantRaster(ta.getPath("rgb.tif"), raster);
        for (const char *index : {"NDVI", "NDRE", "NDWI"})
        {
            const std::string refusal = expectInvalidArgs(rgb, index);
            EXPECT_NE(refusal.find("NIR"), std::string::npos) << index << ": " << refusal;
        }
        EXPECT_NE(expectInvalidArgs(rgb, "NDRE").find("Red edge"), std::string::npos);
        // SAVI and EVI of a Byte raster are refused for its data first (it is not reflectance)
        for (const char *index : {"SAVI", "EVI"})
            EXPECT_NE(expectInvalidArgs(rgb, index).find("reflectance"), std::string::npos) << index;
        // A floating-point RGB raster has the data, and not the bands
        raster.type = GDT_Float32;
        raster.values = {0.1, 0.2, 0.3};
        const fs::path rgb32 = makeConstantRaster(ta.getPath("rgb32.tif"), raster);
        for (const char *index : {"SAVI", "EVI"})
            EXPECT_NE(expectInvalidArgs(rgb32, index).find("NIR"), std::string::npos) << index;
        EXPECT_NE(expectInvalidArgs(rgb32, "EVI").find("3 data bands"), std::string::npos);
        EXPECT_TRUE(indexLayerOf(rgb).empty());
    }

    // ---- R2: the formulas with constants are for reflectance (floating-point rasters) -------------

    // A 5-band raster of `type`, unnamed (the layer's legacy order, and "RGBReN" for the engine)
    fs::path makeFiveBands(const fs::path &path, GDALDataType type)
    {
        ConstantRaster raster;
        raster.type = type;
        const double scale = type == GDT_Float32 ? 1.0 : 60000.0;
        raster.values = {0.20 * scale, 0.10 * scale, 0.05 * scale, 0.30 * scale, 0.60 * scale};
        return makeConstantRaster(path, raster);
    }

    json metadataOf(const fs::path &raster, const char *formula = nullptr, const char *filter = "RGBReN")
    {
        char *buffer = nullptr;
        EXPECT_EQ(DDBGetRasterMetadata(raster.string().c_str(), formula, filter, &buffer), DDBERR_NONE) << DDBGetLastError();
        json metadata = json::parse(buffer);
        DDBFree(buffer);
        return metadata;
    }

    const json *algorithm(const json &metadata, const std::string &id)
    {
        for (const json &a : metadata["algorithms"])
            if (a["id"] == id)
                return &a;
        return nullptr;
    }

    TEST_F(RasterIndexLayer, theFormulasWithConstantsAreOfferedForFloatingPointRastersOnly)
    {
        TestArea ta(TEST_NAME, true);
        const json f32 = metadataOf(makeFiveBands(ta.getPath("f32.tif"), GDT_Float32));
        const json u16 = metadataOf(makeFiveBands(ta.getPath("u16.tif"), GDT_UInt16));
        for (const char *id : {"SAVI", "OSAVI", "EVI"})
        {
            SCOPED_TRACE(id);
            ASSERT_NE(algorithm(f32, id), nullptr);
            EXPECT_FALSE(algorithm(f32, id)->contains("disabled")) << algorithm(f32, id)->dump();
            ASSERT_NE(algorithm(u16, id), nullptr);
            EXPECT_TRUE(algorithm(u16, id)->value("disabled", false)) << algorithm(u16, id)->dump();
            const std::string reason = algorithm(u16, id)->value("disabledReason", "");
            EXPECT_NE(reason.find("reflectance"), std::string::npos) << reason;
            EXPECT_NE(reason.find("UInt16"), std::string::npos) << reason;
        }
        // The ratio indices do not depend on the scale of the data
        for (const char *id : {"NDVI", "NDRE", "NDWI", "GNDVI", "VARI", "ARVI"})
        {
            SCOPED_TRACE(id);
            ASSERT_NE(algorithm(u16, id), nullptr);
            EXPECT_FALSE(algorithm(u16, id)->contains("disabled")) << algorithm(u16, id)->dump();
        }
    }

    // Every way a formula is computed asks the same rule: the plan of a render (thumbnails, tiles,
    // exports, the statistics of the metadata and the layer all plan their formula)
    TEST_F(RasterIndexLayer, everyRenderOfAFormulaWithConstantsRefusesAnIntegerRaster)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path f32 = makeFiveBands(ta.getPath("f32.tif"), GDT_Float32);
        const fs::path u16 = makeFiveBands(ta.getPath("u16.tif"), GDT_UInt16);
        const BandFilter filter = VegetationEngine::parseFilter("RGBReN", 5);

        for (const char *id : {"SAVI", "OSAVI", "EVI"})
        {
            SCOPED_TRACE(id);
            std::unique_ptr<GDALDataset> floating(static_cast<GDALDataset *>(GDALOpen(f32.string().c_str(), GA_ReadOnly)));
            std::unique_ptr<GDALDataset> integer(static_cast<GDALDataset *>(GDALOpen(u16.string().c_str(), GA_ReadOnly)));
            EXPECT_NO_THROW(planFormula(GDALDataset::ToHandle(floating.get()), id, filter, "", false));
            try
            {
                planFormula(GDALDataset::ToHandle(integer.get()), id, filter, "", false);
                ADD_FAILURE() << "planned over UInt16";
            }
            catch (const InvalidArgsException &e)
            {
                const std::string message = e.what();
                EXPECT_NE(message.find(std::string(id) + " needs reflectance"), std::string::npos) << message;
                EXPECT_NE(message.find("UInt16"), std::string::npos) << message;
            }

            // The C API: the statistics of the formula, a thumbnail and a tile of the formula
            char *text = nullptr;
            EXPECT_EQ(DDBGetRasterMetadata(f32.string().c_str(), id, "RGBReN", &text), DDBERR_NONE) << DDBGetLastError();
            DDBFree(text);
            text = nullptr;
            // (the C API says DDBERR_EXCEPTION for these exports, with the reason of the refusal)
            EXPECT_NE(DDBGetRasterMetadata(u16.string().c_str(), id, "RGBReN", &text), DDBERR_NONE);
            EXPECT_NE(std::string(DDBGetLastError()).find("reflectance"), std::string::npos) << DDBGetLastError();

            uint8_t *bytes = nullptr;
            int size = 0;
            EXPECT_EQ(DDBGenerateMemoryThumbnailEx(f32.string().c_str(), 64, nullptr, nullptr, id, "RGBReN", nullptr, nullptr,
                                                   &bytes, &size),
                      DDBERR_NONE) << DDBGetLastError();
            DDBVSIFree(bytes);
            bytes = nullptr;
            EXPECT_NE(DDBGenerateMemoryThumbnailEx(u16.string().c_str(), 64, nullptr, nullptr, id, "RGBReN", nullptr, nullptr,
                                                   &bytes, &size),
                      DDBERR_NONE);
            EXPECT_NE(std::string(DDBGetLastError()).find("reflectance"), std::string::npos) << DDBGetLastError();
        }
        // A tile of the formula: a floating-point raster has one, an integer one is refused
        for (const fs::path &raster : {f32, u16})
        {
            GDALTiler tiler(raster.string(), "", 256, false);
            const auto info = tiler.getMinMaxZ();
            const auto bounds = tiler.getMinMaxCoordsForZ(info.max);
            ThumbVisParams visParams;
            visParams.formula = "EVI";
            visParams.bandFilter = "RGBReN";
            uint8_t *bytes = nullptr;
            int size = 0;
            if (raster == f32)
            {
                EXPECT_NO_THROW(tiler.tile(info.max, bounds.min.x, bounds.min.y, visParams, &bytes, &size));
                VSIFree(bytes);
            }
            else
            {
                EXPECT_THROW(tiler.tile(info.max, bounds.min.x, bounds.min.y, visParams, &bytes, &size), InvalidArgsException);
            }
        }
        // And a ratio index over the same integer raster is fine
        {
            std::unique_ptr<GDALDataset> integer(static_cast<GDALDataset *>(GDALOpen(u16.string().c_str(), GA_ReadOnly)));
            EXPECT_NO_THROW(planFormula(GDALDataset::ToHandle(integer.get()), "NDVI", filter, "", false));
        }
    }

    TEST_F(RasterIndexLayer, theLayerRendersSaviAndEviOverFloatsAndRefusesThemOverIntegers)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path f32 = makeFiveBands(ta.getPath("f32.tif"), GDT_Float32);
        const fs::path u16 = makeFiveBands(ta.getPath("u16.tif"), GDT_UInt16);
        const double bbox[4] = {kBbox[0], kBbox[1], kBbox[2], kBbox[3]};
        // R 0.2, B 0.05, N 0.6: SAVI = 0.6 / 1.3, EVI = 1.0 / (0.6 + 1.2 - 0.375 + 1)
        expectRamp(renderWindow(f32, "SAVI", bbox), 64, 64, 0.6 / 1.3);
        expectRamp(renderWindow(f32, "EVI", bbox), 64, 64, 1.0 / 2.425);
        for (const char *index : {"SAVI", "EVI", "savi"})
        {
            const std::string refusal = expectInvalidArgs(u16, index);
            EXPECT_NE(refusal.find("reflectance"), std::string::npos) << refusal;
            EXPECT_NE(refusal.find("UInt16"), std::string::npos) << refusal;
        }
        // The ratio indices render over integers
        expectRamp(renderWindow(u16, "NDVI", bbox), 64, 64, 0.5);
        EXPECT_EQ(indexLayerOf(f32), kAllFive);
        EXPECT_EQ(indexLayerOf(u16), (std::vector<std::string>{"NDVI", "NDRE", "NDWI"}));
    }

    // The alpha band does not count as a data band: a Float32 raster with its alpha band is a floating-point
    // raster, and a UInt16 one with its alpha band is an integer one (its alpha is Float32 here too)
    TEST_F(RasterIndexLayer, theTypeOfTheDataBandsDecidesNotTheAlphaBand)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path f32 = makeMultispectralWithAlpha(ta.getPath("f32.tif"), GDT_Float32, false);
        const fs::path u16 = makeMultispectralWithAlpha(ta.getPath("u16.tif"), GDT_UInt16, false);
        EXPECT_EQ(indexLayerOf(f32), kAllFive);
        EXPECT_EQ(indexLayerOf(u16), (std::vector<std::string>{"NDVI", "NDRE", "NDWI"}));
    }

    // ---- R3 and the alpha band: what the layer makes of the edge of the alpha -----------------------

    ConstantRaster edgeRaster(GDALDataType type, int alphaStart)
    {
        ConstantRaster raster;
        raster.type = type;
        const double scale = type == GDT_Float32 ? 1.0 : 60000.0;
        raster.values = {0.2 * scale, 0.4 * scale, 0.1 * scale, 0.3 * scale, 0.6 * scale};
        raster.alpha = true;
        // The alpha is 0 over the first columns, and the bands are 0 there
        raster.alphaValue = [alphaStart](int x, int) { return x < alphaStart ? 0.0 : 255.0; };
        return raster;
    }





    // The alpha says no data: 0 stays 0 and 255 stays 255 in the three types an alpha has
    TEST_F(RasterIndexLayer, anAlphaOfZeroIsTransparentAndOfItsMaximumOpaque)
    {
        TestArea ta(TEST_NAME, true);
        const double bbox[4] = {kBbox[0], kBbox[1], kBbox[2], kBbox[3]};
        struct Case
        {
            GDALDataType type;
            double maximum;
        };
        for (const Case c : {Case{GDT_Float32, 255.0}, Case{GDT_UInt16, 255.0}, Case{GDT_UInt16, 65535.0}})
        {
            SCOPED_TRACE(std::string(c.type == GDT_Float32 ? "Float32 " : "UInt16 ") + std::to_string(c.maximum));
            ConstantRaster spec = edgeRaster(c.type, 0);
            spec.alphaValue = [m = c.maximum](int x, int y) { return (x >= 64 && y >= 64) ? 0.0 : m; };
            const fs::path raster = makeConstantRaster(ta.getPath("z" + std::to_string(static_cast<int>(c.maximum)) + (c.type == GDT_Float32 ? "f.tif" : "u.tif")), spec);
            for (const char *index : {"NDVI", "NDRE", "NDWI"})
            {
                const testutils::DecodedImage img = renderWindow(raster, index, bbox);
                EXPECT_EQ(pixelOf(img, 20, 20)[3], 255) << index;
                EXPECT_EQ(pixelOf(img, 100, 20)[3], 255) << index;
                EXPECT_EQ(pixelOf(img, 100, 100)[3], 0) << index;
            }
        }
    }

    // A NaN in a band the index reads leaves the pixel transparent, in a band it does not read it does not
    TEST_F(RasterIndexLayer, aNaNBandValueIsTransparentOnlyForTheIndicesThatReadTheBand)
    {
        TestArea ta(TEST_NAME, true);
        ConstantRaster spec;
        spec.values = {0.2, 0.4, 0.1, 0.3, 0.6};
        testutils::GeoRasterSpec geo;
        geo.width = geo.height = dq06::kSize;
        geo.bands = 5;
        geo.type = GDT_Float32;
        geo.epsg = 3857;
        geo.originX = dq06::kOriginX;
        geo.originY = dq06::kOriginY;
        geo.pixelSize = dq06::kPixel;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        // NaN in the red band (1) of the pixel (30, 30) and in the red edge band (4) of (60, 60)
        geo.value = [&](int band, int x, int y) {
            if (band == 1 && x == 30 && y == 30)
                return nan;
            if (band == 4 && x == 60 && y == 60)
                return nan;
            return spec.values[band - 1];
        };
        const fs::path raster = testutils::createGeoRaster(ta.getPath("nan.tif"), geo);
        const double bbox[4] = {kBbox[0], kBbox[1], kBbox[2], kBbox[3]};
        // NDVI reads R and N, NDRE reads Re and N, NDWI reads G and N, SAVI R and N, EVI R, B, N
        const struct
        {
            const char *index;
            bool readsRed, readsRedEdge;
        } cases[] = {{"NDVI", true, false}, {"NDRE", false, true}, {"NDWI", false, false}, {"SAVI", true, false}, {"EVI", true, false}};
        for (const auto &c : cases)
        {
            SCOPED_TRACE(c.index);
            const testutils::DecodedImage img = renderWindow(raster, c.index, bbox);
            EXPECT_EQ(pixelOf(img, 30, 30)[3], c.readsRed ? 0 : 255);
            EXPECT_EQ(pixelOf(img, 60, 60)[3], c.readsRedEdge ? 0 : 255);
            EXPECT_EQ(pixelOf(img, 31, 30)[3], 255);
        }
    }

}
