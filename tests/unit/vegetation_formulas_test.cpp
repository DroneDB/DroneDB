/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The arithmetic of every formula of the vegetation engine, against values worked out by hand: the
// inputs and the expected outputs are literals of this file, none comes from the engine. Each
// expected value is written with its working. Also: what a formula does with a zero denominator,
// a NaN and a nodata sample.
//
// Two sets of band values, in reflectance:
//   A  R 0.2   G 0.4   B 0.1   N 0.6   Re 0.3
//   B  R 0.1   G 0.3   B 0.2   N 0.5   Re 0.4

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"
#include "raster_test_utils.h"

#include "ddb.h"
#include "gdaltiler.h"
#include "tilerhelper.h"
#include "raster_utils.h"
#include "vegetation.h"
#include "vis_render.h"

namespace
{
    using namespace ddb;

    constexpr float kNoData = NODATA_SENTINEL;
    constexpr float kTolerance = 1e-5f;
    const float kNaN = std::numeric_limits<float>::quiet_NaN();

    class VegetationFormulas : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    // Bands in the order R, G, B, N, Re, and the thermal band after them
    BandFilter filterOfAllBands()
    {
        BandFilter filter;
        filter.R = 0;
        filter.G = 1;
        filter.B = 2;
        filter.N = 3;
        filter.Re = 4;
        filter.T = 5;
        return filter;
    }

    struct Pixel
    {
        float r, g, b, n, re, t;
    };

    // The value of the formula for each pixel
    std::vector<float> compute(const std::string &id, const std::vector<Pixel> &pixels)
    {
        const VegetationFormula *formula = VegetationEngine::instance().getFormula(id);
        EXPECT_NE(formula, nullptr) << id;
        std::vector<std::vector<float>> bands(6, std::vector<float>(pixels.size()));
        for (size_t i = 0; i < pixels.size(); i++)
        {
            bands[0][i] = pixels[i].r;
            bands[1][i] = pixels[i].g;
            bands[2][i] = pixels[i].b;
            bands[3][i] = pixels[i].n;
            bands[4][i] = pixels[i].re;
            bands[5][i] = pixels[i].t;
        }
        std::vector<float *> pointers;
        for (auto &band : bands)
            pointers.push_back(band.data());
        std::vector<float> out(pixels.size(), 12345.0f);
        VegetationEngine::instance().applyFormula(*formula, filterOfAllBands(), pointers, out.data(), pixels.size(),
                                                  kNoData);
        return out;
    }

    const Pixel kA{0.2f, 0.4f, 0.1f, 0.6f, 0.3f, 25.5f};
    const Pixel kB{0.1f, 0.3f, 0.2f, 0.5f, 0.4f, -10.0f};

    // formula -> value for set A, value for set B
    struct Expected
    {
        const char *id;
        float a;
        float b;
    };

    const std::vector<Expected> kExpected = {
        // (G - R) / (G + R - B): A 0.2 / 0.5 = 0.4; B 0.2 / 0.2 = 1
        {"VARI", 0.4f, 1.0f},
        // 2G - (R + B): A 0.8 - 0.3 = 0.5; B 0.6 - 0.3 = 0.3
        {"EXG", 0.5f, 0.3f},
        // (2G - R - B) / (2G + R + B): A 0.5 / 1.1; B 0.3 / 0.9
        {"GLI", 0.45454545f, 0.33333333f},
        // 0.5268 * R^-0.1294 * G^0.3389 * B^-0.3118, with the logarithms (ln 0.1 = -2.302585,
        // ln 0.2 = -1.609438, ln 0.3 = -1.203973, ln 0.4 = -0.916291):
        //   A  exponent = 0.208259 - 0.310531 + 0.717929 = 0.615657 (to 1e-5) -> e^0.61568 = 1.85091 -> * 0.5268 = 0.97506
        //   B  exponent = 0.297955 ... = -0.1294 ln 0.1 + 0.3389 ln 0.3 - 0.3118 ln 0.2
        //              = 0.297955 - 0.408023 + 0.501820 = 0.391752 -> e^0.39175 = 1.47957 -> * 0.5268 = 0.77944
        {"vNDVI", 0.97506f, 0.77944f},
        // (N - R) / (N + R): A 0.4 / 0.8 = 0.5; B 0.4 / 0.6
        {"NDVI", 0.5f, 0.66666667f},
        // (G - N) / (G + N): A -0.2 / 1.0 = -0.2; B -0.2 / 0.8 = -0.25
        {"NDWI", -0.2f, -0.25f},
        // (N - G) / (N + G): A 0.2 / 1.0 = 0.2; B 0.2 / 0.8 = 0.25
        {"GNDVI", 0.2f, 0.25f},
        // 1.5 (N - R) / (N + R + 0.5): A 0.6 / 1.3; B 0.6 / 1.1
        {"SAVI", 0.46153846f, 0.54545455f},
        // 2.5 (N - R) / (N + 6R - 7.5B + 1): A 1.0 / (0.6 + 1.2 - 0.75 + 1 = 2.05); B 1.0 / (0.5 + 0.6 - 1.5 + 1 = 0.6)
        {"EVI", 0.48780488f, 1.66666667f},
        // (N - Re) / (N + Re): A 0.3 / 0.9; B 0.1 / 0.9
        {"NDRE", 0.33333333f, 0.11111111f},
        // (G - B) / (G + B): A 0.3 / 0.5 = 0.6; B 0.1 / 0.5 = 0.2
        {"NDYI", 0.6f, 0.2f},
        // (G - R) / (G + R): A 0.2 / 0.6; B 0.2 / 0.4 = 0.5
        {"MPRI", 0.33333333f, 0.5f},
        // (N - R) / (N + R + 0.16): A 0.4 / 0.96; B 0.4 / 0.76
        {"OSAVI", 0.41666667f, 0.52631579f},
        // N / G: A 0.6 / 0.4 = 1.5; B 0.5 / 0.3
        {"GRVI", 1.5f, 1.66666667f},
        // ((N + G) - 2B) / ((N + G) + 2B): A 0.8 / 1.2; B 0.4 / 1.2
        {"ENDVI", 0.66666667f, 0.33333333f},
        // (N - 2R + B) / (N + 2R + B): A 0.3 / 1.1; B 0.5 / 0.9
        {"ARVI", 0.27272727f, 0.55555556f},
    };

    // The thermal passthroughs read band 0 (a single-band thermal raster), whatever the filter says: the
    // value of the pixel in degrees Celsius, and + 273.15 for Kelvin
    TEST_F(VegetationFormulas, theThermalFormulasReadBandZero)
    {
        const std::vector<Pixel> pixels = {{25.5f, 0, 0, 0, 0, 0}, {-10.0f, 0, 0, 0, 0, 0}};
        const std::vector<float> celsius = compute("CELSIUS", pixels);
        EXPECT_NEAR(celsius[0], 25.5f, kTolerance);
        EXPECT_NEAR(celsius[1], -10.0f, kTolerance);
        const std::vector<float> kelvin = compute("KELVIN", pixels);
        EXPECT_NEAR(kelvin[0], 298.65f, 1e-4f); // 25.5 + 273.15
        EXPECT_NEAR(kelvin[1], 263.15f, 1e-4f); // -10 + 273.15
    }

    TEST_F(VegetationFormulas, everyFormulaOfTheTableIsCheckedByHand)
    {
        std::map<std::string, bool> covered;
        for (const VegetationFormula &formula : VegetationEngine::instance().getAllFormulas())
            covered[formula.id] = false;
        covered["CELSIUS"] = covered["KELVIN"] = true; // theThermalFormulasReadBandZero
        for (const Expected &e : kExpected)
        {
            SCOPED_TRACE(e.id);
            ASSERT_TRUE(covered.count(e.id)) << "a formula that is not in the table";
            covered[e.id] = true;
            const std::vector<float> out = compute(e.id, {kA, kB});
            EXPECT_NEAR(out[0], e.a, kTolerance);
            EXPECT_NEAR(out[1], e.b, kTolerance);
        }
        for (const auto &[id, checked] : covered)
            EXPECT_TRUE(checked) << id << " has no hand-computed expectation";
    }

    // A denominator of zero has no value. Every operand below is exact in binary, so the
    // denominator is exactly 0
    TEST_F(VegetationFormulas, aZeroDenominatorIsNoData)
    {
        const Pixel zero{0, 0, 0, 0, 0, 0};
        // All the bands 0: the denominators of ... (those with no constant in them)
        for (const char *id : {"VARI", "GLI", "NDVI", "NDWI", "GNDVI", "NDRE", "NDYI", "MPRI", "GRVI", "ENDVI", "ARVI",
                               "vNDVI"})
        {
            SCOPED_TRACE(id);
            EXPECT_EQ(compute(id, {zero})[0], kNoData);
        }
        // ... where there is a constant, 0 is a value: 1.5 * 0 / 0.5, 2.5 * 0 / 1, 0 / 0.16
        for (const char *id : {"SAVI", "EVI", "OSAVI", "EXG"})
        {
            SCOPED_TRACE(id);
            EXPECT_EQ(compute(id, {zero})[0], 0.0f);
        }
        // SAVI: N + R + 0.5 = -0.25 - 0.25 + 0.5
        EXPECT_EQ(compute("SAVI", {{-0.25f, 0, 0, -0.25f, 0, 0}})[0], kNoData);
        // EVI: N + 6R - 7.5B + 1 = 1.25 + 1.5 - 3.75 + 1
        EXPECT_EQ(compute("EVI", {{0.25f, 0, 0.5f, 1.25f, 0, 0}})[0], kNoData);
        // OSAVI: N + R + 0.16 = -0.16 + 0 + 0.16
        EXPECT_EQ(compute("OSAVI", {{0, 0, 0, -0.16f, 0, 0}})[0], kNoData);
        // VARI: G + R - B = 0.25 + 0.25 - 0.5
        EXPECT_EQ(compute("VARI", {{0.25f, 0.25f, 0.5f, 0, 0, 0}})[0], kNoData);
        // NDVI: N + R = 0.5 - 0.5, and the pixel next to it is a value
        const std::vector<float> ndvi = compute("NDVI", {{-0.5f, 0, 0, 0.5f, 0, 0}, kA});
        EXPECT_EQ(ndvi[0], kNoData);
        EXPECT_NEAR(ndvi[1], 0.5f, kTolerance);
    }

    // The formulas that read a band do not give a value to a pixel where it is NaN (or infinite): no
    // number is rendered, counted or averaged for it
    TEST_F(VegetationFormulas, aNaNInAnyBandTheFormulaReadsIsNoData)
    {
        struct Case
        {
            const char *id;
            Pixel pixel; // A with one band NaN
        };
        const float inf = std::numeric_limits<float>::infinity();
        const std::vector<Case> cases = {
            {"NDVI", {kNaN, kA.g, kA.b, kA.n, kA.re, kA.t}},       {"NDVI", {kA.r, kA.g, kA.b, kNaN, kA.re, kA.t}},
            {"NDRE", {kA.r, kA.g, kA.b, kA.n, kNaN, kA.t}},        {"NDWI", {kA.r, kNaN, kA.b, kA.n, kA.re, kA.t}},
            {"SAVI", {kNaN, kA.g, kA.b, kA.n, kA.re, kA.t}},       {"EVI", {kA.r, kA.g, kNaN, kA.n, kA.re, kA.t}},
            {"VARI", {kA.r, kA.g, kNaN, kA.n, kA.re, kA.t}},       {"EXG", {kA.r, kNaN, kA.b, kA.n, kA.re, kA.t}},
            {"GLI", {kNaN, kA.g, kA.b, kA.n, kA.re, kA.t}},        {"vNDVI", {kA.r, kA.g, kNaN, kA.n, kA.re, kA.t}},
            {"GNDVI", {kA.r, kNaN, kA.b, kA.n, kA.re, kA.t}},      {"NDYI", {kA.r, kA.g, kNaN, kA.n, kA.re, kA.t}},
            {"MPRI", {kNaN, kA.g, kA.b, kA.n, kA.re, kA.t}},       {"OSAVI", {kA.r, kA.g, kA.b, kNaN, kA.re, kA.t}},
            {"GRVI", {kA.r, kNaN, kA.b, kA.n, kA.re, kA.t}},       {"ENDVI", {kA.r, kA.g, kNaN, kA.n, kA.re, kA.t}},
            {"ARVI", {kA.r, kA.g, kA.b, kNaN, kA.re, kA.t}},       {"NDVI", {inf, kA.g, kA.b, kA.n, kA.re, kA.t}},
        };
        for (const Case &c : cases)
        {
            SCOPED_TRACE(c.id);
            const std::vector<float> out = compute(c.id, {c.pixel, kA});
            EXPECT_EQ(out[0], kNoData);
            EXPECT_TRUE(std::isfinite(out[1])) << "the pixel next to it is a value";
        }
        // The thermal passthroughs read band 0
        for (const char *id : {"CELSIUS", "KELVIN"})
        {
            SCOPED_TRACE(id);
            EXPECT_EQ(compute(id, {{kNaN, 0, 0, 0, 0, 0}})[0], kNoData);
        }
    }

    // A sample equal to the nodata value of the engine is no data too (the caller of the engine passes
    // null for the bands that a formula does not read, so every band given decides)
    TEST_F(VegetationFormulas, aBandNoDataIsNoData)
    {
        EXPECT_EQ(compute("NDVI", {{kNoData, kA.g, kA.b, kA.n, kA.re, kA.t}})[0], kNoData);
        EXPECT_EQ(compute("NDVI", {{kA.r, kA.g, kA.b, kNoData, kA.re, kA.t}})[0], kNoData);
        EXPECT_EQ(compute("EVI", {{kA.r, kA.g, kNoData, kA.n, kA.re, kA.t}})[0], kNoData);
        EXPECT_NEAR(compute("NDVI", {kA})[0], 0.5f, kTolerance);
    }

    // The same, through the pipeline that reads a raster: a band nodata value (here -1, declared) and a NaN
    // pixel leave a pixel out; the values are those of the hand computation above
    TEST_F(VegetationFormulas, theRasterPipelineLeavesOutNodataAndNaNPixels)
    {
        TestArea ta(TEST_NAME, true);
        // Four pixels in a row, the bands in the order R G B Re N: A, A with nodata (-1) as R, A with
        // a NaN as N, B
        const std::vector<std::vector<double>> planes = {
            {0.2, -1.0, 0.2, 0.1},                                            // R
            {0.4, 0.4, 0.4, 0.3},                                             // G
            {0.1, 0.1, 0.1, 0.2},                                             // B
            {0.3, 0.3, 0.3, 0.4},                                             // Re
            {0.6, 0.6, std::numeric_limits<double>::quiet_NaN(), 0.5}};       // N
        testutils::GeoRasterSpec spec;
        spec.width = 4;
        spec.height = 1;
        spec.bands = 5;
        spec.type = GDT_Float32;
        spec.epsg = 3857;
        spec.value = [&](int band, int x, int) { return planes[band - 1][x]; };
        const fs::path raster = testutils::createGeoRaster(ta.getPath("row.tif"), spec);
        {
            std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(raster.string().c_str(), GA_Update)));
            ASSERT_NE(ds, nullptr);
            for (int b = 1; b <= 5; b++)
                ds->GetRasterBand(b)->SetNoDataValue(-1.0);
        }
        std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(raster.string().c_str(), GA_ReadOnly)));
        ASSERT_NE(ds, nullptr);
        GDALDatasetH handle = GDALDataset::ToHandle(ds.get());
        const RasterWindow window{0, 0, 4, 1, 4, 1};
        // Both rules of the engine
        for (const NodataMask mask : {NodataMask::AnyBand, NodataMask::BandsRead})
        {
            const FormulaPlan plan = planFormula(handle, "NDVI", VegetationEngine::parseFilter("RGBReN", 5), "", false);
            FormulaBuffers buffers;
            computeFormulaValues(handle, plan, window, buffers, mask);
            ASSERT_EQ(buffers.result.size(), 4u);
            // (N - R) / (N + R): 0.4 / 0.8, then no value (R is nodata), no value (N is NaN), 0.4 / 0.6
            EXPECT_NEAR(buffers.result[0], 0.5f, kTolerance);
            EXPECT_EQ(buffers.result[1], kNoData);
            EXPECT_EQ(buffers.result[2], kNoData);
            EXPECT_NEAR(buffers.result[3], 0.66666667f, kTolerance);
        }
        // NDRE does not read R: the nodata R of pixel 2 is no obstacle to it under BandsRead, and it is
        // under AnyBand, where every band decides. The NaN N is one for both
        const FormulaPlan ndre = planFormula(handle, "NDRE", VegetationEngine::parseFilter("RGBReN", 5), "", false);
        FormulaBuffers buffers;
        computeFormulaValues(handle, ndre, window, buffers, NodataMask::BandsRead);
        EXPECT_NEAR(buffers.result[0], 0.33333333f, kTolerance); // 0.3 / 0.9
        EXPECT_NEAR(buffers.result[1], 0.33333333f, kTolerance);
        EXPECT_EQ(buffers.result[2], kNoData);
        EXPECT_NEAR(buffers.result[3], 0.11111111f, kTolerance); // 0.1 / 0.9
        computeFormulaValues(handle, ndre, window, buffers, NodataMask::AnyBand);
        EXPECT_NEAR(buffers.result[0], 0.33333333f, kTolerance);
        EXPECT_EQ(buffers.result[1], kNoData);
        EXPECT_EQ(buffers.result[2], kNoData);
    }

    // A raster whose transparency is a GDAL dataset mask (an internal TIFF mask, as JPEG-compressed COGs
    // carry it) and not an alpha band or a nodata value: the pixels the mask says 0 are left out of the
    // formula, under both rules of the engine and in the render. They read 0 in every band, which EXG
    // (2G - (R + B)) turns into the value 0 and not into nodata, as a ratio would
    TEST_F(VegetationFormulas, theRasterPipelineLeavesOutThePixelsOfTheDatasetMask)
    {
        TestArea ta(TEST_NAME, true);
        // Four pixels in a row, R G B: black outside the mask, a value, black inside it, a value
        const std::vector<std::vector<double>> planes = {{0, 10, 0, 40}, {0, 50, 0, 30}, {0, 20, 0, 10}};
        const std::vector<uint8_t> maskValues = {0, 255, 255, 255};
        testutils::GeoRasterSpec spec;
        spec.width = 4;
        spec.height = 1;
        spec.bands = 3;
        spec.type = GDT_Byte;
        spec.value = [&](int band, int x, int) { return planes[band - 1][x]; };

        const fs::path masked = testutils::createGeoRaster(ta.getPath("masked.tif"), spec);
        {
            std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(masked.string().c_str(), GA_Update)));
            ASSERT_NE(ds, nullptr);
            ASSERT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_None);
            std::vector<uint8_t> values = maskValues;
            ASSERT_EQ(ds->GetRasterBand(1)->GetMaskBand()->RasterIO(GF_Write, 0, 0, 4, 1, values.data(), 4, 1, GDT_Byte, 0, 0),
                      CE_None);
        }
        const fs::path unmasked = testutils::createGeoRaster(ta.getPath("unmasked.tif"), spec);

        const RasterWindow window{0, 0, 4, 1, 4, 1};
        const auto open = [](const fs::path &path) {
            return std::unique_ptr<GDALDataset>(static_cast<GDALDataset *>(GDALOpen(path.string().c_str(), GA_ReadOnly)));
        };

        const auto maskedDs = open(masked);
        ASSERT_NE(maskedDs, nullptr);
        GDALDatasetH handle = GDALDataset::ToHandle(maskedDs.get());
        const RasterBandNodata nodata = detectBandNodata(handle, 3);
        EXPECT_TRUE(nodata.datasetMask);
        EXPECT_EQ(nodata.alphaBandIdx, -1);
        for (const NodataMask mask : {NodataMask::AnyBand, NodataMask::BandsRead})
        {
            const FormulaPlan plan = planFormula(handle, "EXG", VegetationEngine::parseFilter("RGB", 3), "", false);
            FormulaBuffers buffers;
            computeFormulaValues(handle, plan, window, buffers, mask);
            ASSERT_EQ(buffers.result.size(), 4u);
            EXPECT_EQ(buffers.result[0], kNoData);
            EXPECT_NEAR(buffers.result[1], 70.0f, kTolerance); // 100 - 30
            EXPECT_NEAR(buffers.result[2], 0.0f, kTolerance);  // black inside the mask is a value
            EXPECT_NEAR(buffers.result[3], 10.0f, kTolerance); // 60 - 50
        }
        ThumbVisParams vis;
        vis.formula = "EXG";
        vis.bandFilter = "RGB";
        vis.rescale = "0,70";
        const auto rgba = renderFormulaRgba(handle, masked.string(), window, vis);
        ASSERT_EQ(rgba.size(), 16u);
        EXPECT_EQ(rgba[3], 0);
        for (size_t i = 1; i < 4; i++)
            EXPECT_EQ(rgba[i * 4 + 3], 255) << "pixel " << i;

        // Without a mask, every pixel is valid: black is the value 0
        const auto unmaskedDs = open(unmasked);
        ASSERT_NE(unmaskedDs, nullptr);
        GDALDatasetH plain = GDALDataset::ToHandle(unmaskedDs.get());
        EXPECT_FALSE(detectBandNodata(plain, 3).datasetMask);
        const FormulaPlan plan = planFormula(plain, "EXG", VegetationEngine::parseFilter("RGB", 3), "", false);
        FormulaBuffers buffers;
        computeFormulaValues(plain, plan, window, buffers);
        EXPECT_NEAR(buffers.result[0], 0.0f, kTolerance);
    }

    // The formulas a raster is offered: the bands the filter maps and the data it can take. The formulas that
    // add constants to the band values (SAVI, OSAVI, EVI) are for reflectance, so for floating-point data only
    TEST_F(VegetationFormulas, theListOfFormulasFollowsTheBandsAndTheTypeOfTheData)
    {
        const VegetationEngine &engine = VegetationEngine::instance();
        const BandFilter all = filterOfAllBands();
        const RasterDataKind floating{"Float32", true, 5};
        const RasterDataKind integer{"UInt16", false, 5};

        const auto ids = [&](const BandFilter &filter, const RasterDataKind &data) {
            std::vector<std::string> out;
            for (const VegetationFormula &f : availableFormulas(filter, data))
                out.push_back(f.id);
            return out;
        };
        const auto has = [](const std::vector<std::string> &list, const char *id) {
            return std::find(list.begin(), list.end(), id) != list.end();
        };
        const std::vector<std::string> onFloats = ids(all, floating), onIntegers = ids(all, integer);
        for (const char *id : {"SAVI", "OSAVI", "EVI"})
        {
            EXPECT_TRUE(has(onFloats, id)) << id;
            EXPECT_FALSE(has(onIntegers, id)) << id;
            EXPECT_TRUE(engine.getFormula(id)->needsReflectance) << id;
        }
        for (const char *id : {"NDVI", "NDRE", "NDWI", "GNDVI", "VARI", "EXG", "GLI", "vNDVI", "NDYI", "MPRI", "GRVI", "ENDVI", "ARVI"})
        {
            EXPECT_TRUE(has(onFloats, id)) << id;
            EXPECT_TRUE(has(onIntegers, id)) << id;
            EXPECT_FALSE(engine.getFormula(id)->needsReflectance) << id;
        }
        EXPECT_EQ(onFloats.size(), onIntegers.size() + 3);

        // The bands rule is unchanged: a filter without blue has no EVI even over floats
        const BandFilter noBlue = VegetationEngine::parseFilter("GRReN", 4);
        EXPECT_FALSE(has(ids(noBlue, floating), "EVI"));
        EXPECT_TRUE(has(ids(noBlue, floating), "SAVI"));
        EXPECT_EQ(VegetationEngine::unavailableReason(*engine.getFormula("EVI"), noBlue, floating), "Requires bands: RBN");
        EXPECT_EQ(VegetationEngine::unavailableReason(*engine.getFormula("EVI"), all, floating), "");
        const std::string reason = VegetationEngine::unavailableReason(*engine.getFormula("EVI"), all, integer);
        EXPECT_EQ(reason, "EVI needs reflectance values (0 to 1, a floating-point raster): the raster is UInt16, whose values are not reflectance");
    }

    // What a raster is asked before a formula is planned (thumbnails, tiles, exports and the statistics of
    // the metadata all plan through planFormula): SAVI, OSAVI and EVI over UInt16 are refused, over
    // Float32 they are planned, and the ratio indices are planned over both. The alpha band is not data
    TEST_F(VegetationFormulas, aFormulaWithConstantsIsPlannedOverFloatingPointDataOnly)
    {
        TestArea ta(TEST_NAME, true);
        const auto make = [&](const std::string &name, GDALDataType type, bool alpha) {
            testutils::GeoRasterSpec spec;
            spec.width = 4;
            spec.height = 1;
            spec.bands = alpha ? 6 : 5;
            spec.alphaBand = alpha;
            spec.type = type;
            spec.value = [](int band, int, int) { return 0.1 * band; };
            return testutils::createGeoRaster(ta.getPath(name), spec);
        };
        const BandFilter filter = VegetationEngine::parseFilter("RGBReN", 5);
        const auto plans = [&](const fs::path &raster, const char *id) {
            std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(raster.string().c_str(), GA_ReadOnly)));
            EXPECT_NE(ds, nullptr);
            return planFormula(GDALDataset::ToHandle(ds.get()), id, filter, "", false).formula->id;
        };
        const fs::path f32 = make("f32.tif", GDT_Float32, false);
        const fs::path u16 = make("u16.tif", GDT_UInt16, false);
        const fs::path u16Alpha = make("u16alpha.tif", GDT_UInt16, true);
        const fs::path f32Alpha = make("f32alpha.tif", GDT_Float32, true);

        for (const char *id : {"SAVI", "OSAVI", "EVI"})
        {
            SCOPED_TRACE(id);
            EXPECT_EQ(plans(f32, id), id);
            EXPECT_EQ(plans(f32Alpha, id), id);
            for (const fs::path &integer : {u16, u16Alpha})
            {
                try
                {
                    plans(integer, id);
                    ADD_FAILURE() << "planned over " << integer;
                }
                catch (const InvalidArgsException &e)
                {
                    EXPECT_EQ(std::string(e.what()), std::string(id) + " needs reflectance values (0 to 1, a floating-point raster): the raster is UInt16, whose values are not reflectance");
                }
            }
        }
        for (const char *id : {"NDVI", "NDRE", "GNDVI", "VARI"})
        {
            EXPECT_EQ(plans(u16, id), id);
            EXPECT_EQ(plans(f32, id), id);
        }

        // The raster of the metadata lists them as disabled, with the reason
        char *buffer = nullptr;
        ASSERT_EQ(DDBGetRasterMetadata(u16.string().c_str(), nullptr, "RGBReN", &buffer), DDBERR_NONE) << DDBGetLastError();
        const json metadata = json::parse(buffer);
        DDBFree(buffer);
        for (const json &algorithm : metadata["algorithms"])
        {
            const std::string id = algorithm["id"];
            if (id == "SAVI" || id == "OSAVI" || id == "EVI")
            {
                EXPECT_TRUE(algorithm.value("disabled", false)) << id;
                EXPECT_NE(algorithm.value("disabledReason", "").find("reflectance"), std::string::npos) << id;
            }
            else if (id == "NDVI" || id == "NDRE" || id == "NDWI")
            {
                EXPECT_FALSE(algorithm.contains("disabled")) << id;
            }
        }
    }

    // A formula whose bands the raster does not have is refused by planFormula, as the metadata
    // marks it disabled, with the same reason: NDVI over a 3-band RGB orthophoto is "Requires
    // bands: RN" for the render, the tiles, the export and the statistics, not a one-colour image.
    // VARI, which reads R, G and B, is planned and rendered; an unknown formula is "Unknown formula"
    TEST_F(VegetationFormulas, aFormulaWhoseBandsAreMissingIsRefusedWithItsReason)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = 8;
        spec.height = 8;
        spec.bands = 3;
        spec.type = GDT_Byte;
        spec.value = [](int band, int x, int y) { return 40.0 * band + x + y; };
        const fs::path rgb = testutils::createGeoRaster(ta.getPath("rgb.tif"), spec);

        std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(rgb.string().c_str(), GA_ReadOnly)));
        ASSERT_NE(ds, nullptr);
        GDALDatasetH handle = GDALDataset::ToHandle(ds.get());
        const std::string path = rgb.string();

        const auto refusal = [&](const auto &plan) -> std::string {
            try
            {
                plan();
            }
            catch (const InvalidArgsException &e)
            {
                return e.what();
            }
            return "";
        };

        // The band filter detected from the raster (the render, the tiles, the export) and one given
        EXPECT_EQ(refusal([&] { planFormula(handle, path, "NDVI", "", "", true); }), "Requires bands: RN");
        EXPECT_EQ(refusal([&] { planFormula(handle, "NDVI", VegetationEngine::parseFilter("RGB", 3), "", false); }),
                  "Requires bands: RN");
        ThumbVisParams ndvi;
        ndvi.formula = "NDVI";
        const RasterWindow window{0, 0, 8, 8, 8, 8};
        EXPECT_EQ(refusal([&] { renderFormulaRgba(handle, path, window, ndvi); }), "Requires bands: RN");

        EXPECT_EQ(planFormula(handle, path, "VARI", "", "", true).formula->id, "VARI");
        ThumbVisParams vari;
        vari.formula = "VARI";
        EXPECT_EQ(renderFormulaRgba(handle, path, window, vari).size(), window.outPixels() * 4);

        EXPECT_EQ(refusal([&] { planFormula(handle, path, "BOGUS", "", "", true); }), "Unknown formula: BOGUS");
        ds.reset();

        // The statistics of the metadata plan the formula too: the same reason, not numbers
        char *buffer = nullptr;
        EXPECT_NE(DDBGetRasterMetadata(path.c_str(), "NDVI", nullptr, &buffer), DDBERR_NONE);
        EXPECT_NE(std::string(DDBGetLastError()).find("Requires bands: RN"), std::string::npos) << DDBGetLastError();
        ASSERT_EQ(DDBGetRasterMetadata(path.c_str(), nullptr, nullptr, &buffer), DDBERR_NONE) << DDBGetLastError();
        const json metadata = json::parse(buffer);
        DDBFree(buffer);
        for (const json &algorithm : metadata["algorithms"]) {
            if (algorithm["id"] == "NDVI") {
                EXPECT_EQ(algorithm.value("disabledReason", ""), "Requires bands: RN");
            }
        }
    }

    // What a caller of the C ABI gets for a formula whose bands the raster does not have: the
    // plain reason as the last error, with no path, from the metadata, the tiles, the exports and
    // the thumbnail. The metadata, the tiles and the exports plan the formula in the process and
    // return DDBERR_INVALIDARGS (RefusedArgumentException, which every export returns). The
    // thumbnail is rendered by gdal-util, whose protocol has no error code for an invalid
    // argument yet: it is DDBERR_EXCEPTION, with the same message
    TEST_F(VegetationFormulas, theCAbiRefusesAFormulaWhoseBandsAreMissingWithThePlainReason)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = 64;
        spec.height = 64;
        spec.bands = 3;
        spec.type = GDT_Byte;
        spec.value = [](int band, int x, int y) { return 40.0 * band + x + y; };
        const std::string path = testutils::createGeoRaster(ta.getPath("rgb.tif"), spec).string();
        const std::string exported = ta.getPath("out.tif").string();

        uint8_t *bytes = nullptr;
        int size = 0;
        char *text = nullptr;
        struct Call
        {
            std::string name;
            std::function<DDBErr()> call;
            DDBErr expected = DDBERR_INVALIDARGS;
        };
        const std::vector<Call> calls = {
            {"metadata", [&] { return DDBGetRasterMetadata(path.c_str(), "NDVI", nullptr, &text); }},
            {"tile", [&] { return DDBMemoryTileEx(path.c_str(), 14, 8192, 8192, 256, false, true, nullptr, nullptr, nullptr,
                                                  "NDVI", nullptr, nullptr, nullptr, &bytes, &size); }},
            {"export", [&] { return DDBExportRaster(path.c_str(), exported.c_str(), nullptr, nullptr, "NDVI", nullptr, nullptr, nullptr); }},
            {"export2", [&] { return DDBExportRaster2(path.c_str(), exported.c_str(), nullptr, nullptr, "NDVI", nullptr, nullptr,
                                                     nullptr, 256, nullptr, nullptr); }},
            {"thumbnail", [&] { return DDBGenerateMemoryThumbnailEx(path.c_str(), 64, nullptr, nullptr, "NDVI", nullptr, nullptr,
                                                                    nullptr, &bytes, &size); },
             DDBERR_EXCEPTION},
        };
        for (const auto &c : calls)
        {
            EXPECT_EQ(c.call(), c.expected) << c.name;
            EXPECT_EQ(std::string(DDBGetLastError()), "Requires bands: RN") << c.name;
        }
        EXPECT_FALSE(fs::exists(exported));

        EXPECT_EQ(DDBGetRasterMetadata(path.c_str(), "BOGUS", nullptr, &text), DDBERR_INVALIDARGS);
        EXPECT_EQ(std::string(DDBGetLastError()), "Unknown formula: BOGUS");
    }

    // CELSIUS and KELVIN read the first band of the raster whatever the band filter maps, so a
    // render plans them over a single-band Float32 raster that is not thermal (a DEM, which the
    // Hub colourises with CELSIUS), whose detected filter is empty: no "Requires bands: T". The list
    // of formulas keeps them disabled there, and NDVI over RGB is still refused
    TEST_F(VegetationFormulas, theThermalPassthroughsRenderTheFirstBandOfANonThermalRaster)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = 64;
        spec.height = 64;
        spec.bands = 1;
        spec.type = GDT_Float32;
        spec.value = [](int, int x, int y) { return 120.0 + 0.5 * x + 0.25 * y; };
        const std::string dem = testutils::createGeoRaster(ta.getPath("dem.tif"), spec).string();
        // A raster of one band has no red, green and blue to map (FR-RAS-19): its filter is empty, and the passthroughs
        // still plan, as they read the first band whatever the filter maps
        ASSERT_EQ(VegetationEngine::instance().autoDetectFilter(dem).id, "");

        {
            std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(dem.c_str(), GA_ReadOnly)));
            ASSERT_NE(ds, nullptr);
            GDALDatasetH handle = GDALDataset::ToHandle(ds.get());
            const RasterWindow window{0, 0, 64, 64, 64, 64};
            for (const char *id : {"CELSIUS", "KELVIN"})
            {
                SCOPED_TRACE(id);
                EXPECT_EQ(planFormula(handle, dem, id, "", "", true).formula->id, id);
                ThumbVisParams vis;
                vis.formula = id;
                const std::vector<uint8_t> rgba = renderFormulaRgba(handle, dem, window, vis);
                ASSERT_EQ(rgba.size(), window.outPixels() * 4);
                EXPECT_EQ(rgba[3], 255); // computed, not transparent
                EXPECT_NE(std::vector<uint8_t>(rgba.begin(), rgba.begin() + 3),
                          std::vector<uint8_t>(rgba.end() - 4, rgba.end() - 1)); // not one colour
            }
        }

        char *text = nullptr;
        ASSERT_EQ(DDBGetRasterMetadata(dem.c_str(), "CELSIUS", nullptr, &text), DDBERR_NONE) << DDBGetLastError();
        const json metadata = json::parse(text);
        DDBFree(text);
        EXPECT_TRUE(metadata["statistics"]["1"].contains("min"));
        for (const json &algorithm : metadata["algorithms"]) {
            if (algorithm["id"] == "CELSIUS" || algorithm["id"] == "KELVIN") {
                EXPECT_EQ(algorithm.value("disabledReason", ""), "Requires bands: T");
            }
        }

        // A tile of the raster at its deepest zoom level (the tiler that gives it is closed first)
        const auto [zoom, tile] = [&] {
            GDALTiler tiler(TilerHelper::toGeoTIFF(dem).string(), "", 256, false);
            const auto z = tiler.getMinMaxZ();
            return std::make_pair(z.max, tiler.getMinMaxCoordsForZ(z.max).min);
        }();
        uint8_t *bytes = nullptr;
        int size = 0;
        ASSERT_EQ(DDBMemoryTileEx(dem.c_str(), zoom, tile.x, tile.y, 256, false, true, nullptr, nullptr, nullptr, "KELVIN",
                                  nullptr, nullptr, nullptr, &bytes, &size),
                  DDBERR_NONE)
            << DDBGetLastError();
        EXPECT_GT(size, 0);
        DDBVSIFree(bytes);

        // The other formulas keep the band rule
        spec.bands = 3;
        spec.type = GDT_Byte;
        const std::string rgb = testutils::createGeoRaster(ta.getPath("rgb.tif"), spec).string();
        EXPECT_EQ(DDBGetRasterMetadata(rgb.c_str(), "NDVI", nullptr, &text), DDBERR_INVALIDARGS);
        EXPECT_EQ(std::string(DDBGetLastError()), "Requires bands: RN");
    }

    // A raster of one data band has no red, green and blue (FR-RAS-19): the RGB indices are listed disabled, each with
    // the bands it needs, and a render of one is refused with the same reason instead of an image of nothing; a raster
    // of three bands keeps them
    TEST_F(VegetationFormulas, theRgbIndicesAreDisabledOnASingleBandRaster)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = 16;
        spec.height = 16;
        spec.bands = 1;
        spec.type = GDT_UInt16;
        spec.value = [](int, int x, int y) { return 100.0 + x + y; };
        const std::string single = testutils::createGeoRaster(ta.getPath("single.tif"), spec).string();
        EXPECT_EQ(VegetationEngine::instance().autoDetectFilter(single).id, "");

        const auto reasons = [](const std::string &path) {
            char *text = nullptr;
            EXPECT_EQ(DDBGetRasterMetadata(path.c_str(), nullptr, nullptr, &text), DDBERR_NONE) << DDBGetLastError();
            const json metadata = json::parse(text);
            DDBFree(text);
            std::map<std::string, std::string> result;
            for (const json &algorithm : metadata["algorithms"])
                result[algorithm["id"]] = algorithm.value("disabledReason", "");
            return result;
        };

        const auto one = reasons(single);
        const std::map<std::string, std::string> expected = {{"VARI", "Requires bands: RGB"},  {"EXG", "Requires bands: RGB"},
                                                             {"GLI", "Requires bands: RGB"},   {"vNDVI", "Requires bands: RGB"},
                                                             {"NDYI", "Requires bands: GB"},   {"MPRI", "Requires bands: RG"},
                                                             {"NDVI", "Requires bands: RN"}};
        for (const auto &[id, reason] : expected)
            EXPECT_EQ(one.at(id), reason) << id;

        // The render of one is refused, with that reason
        std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(single.c_str(), GA_ReadOnly)));
        ASSERT_NE(ds, nullptr);
        try
        {
            planFormula(GDALDataset::ToHandle(ds.get()), single, "VARI", "", "", true);
            ADD_FAILURE() << "VARI planned over a single band";
        }
        catch (const InvalidArgsException &e)
        {
            EXPECT_EQ(std::string(e.what()), "Requires bands: RGB");
        }
        ds.reset();

        spec.bands = 3;
        spec.type = GDT_Byte;
        const auto three = reasons(testutils::createGeoRaster(ta.getPath("three.tif"), spec).string());
        EXPECT_EQ(three.at("VARI"), "");
        EXPECT_EQ(three.at("EXG"), "");
        EXPECT_EQ(three.at("NDVI"), "Requires bands: RN");
    }

    // The algorithm list carries what a client needs without parsing a symbol string (FR-RAS-32, FR-RAS-33): the names of
    // the bands the formula reads, and the kind of raster it is for
    TEST_F(VegetationFormulas, theAlgorithmListGivesTheRequiredBandsAndTheCategory)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = 8;
        spec.height = 8;
        spec.bands = 3;
        spec.type = GDT_Byte;
        const std::string path = testutils::createGeoRaster(ta.getPath("rgb.tif"), spec).string();
        char *text = nullptr;
        ASSERT_EQ(DDBGetRasterMetadata(path.c_str(), nullptr, nullptr, &text), DDBERR_NONE) << DDBGetLastError();
        const json metadata = json::parse(text);
        DDBFree(text);

        std::map<std::string, json> byId;
        for (const json &algorithm : metadata["algorithms"])
        {
            // Every formula has both, never empty
            ASSERT_TRUE(algorithm["requiredBands"].is_array()) << algorithm["id"];
            EXPECT_FALSE(algorithm["requiredBands"].empty()) << algorithm["id"];
            ASSERT_TRUE(algorithm["category"].is_string()) << algorithm["id"];
            byId[algorithm["id"]] = algorithm;
        }
        EXPECT_EQ(byId.size(), 18u);

        const auto bands = [&](const char *id) { return byId.at(id)["requiredBands"]; };
        const auto category = [&](const char *id) { return byId.at(id)["category"].get<std::string>(); };
        EXPECT_EQ(bands("VARI"), json::array({"Red", "Green", "Blue"}));
        EXPECT_EQ(category("VARI"), "rgb");
        EXPECT_EQ(bands("NDVI"), json::array({"Red", "NIR"}));
        EXPECT_EQ(category("NDVI"), "multispectral");
        EXPECT_EQ(bands("NDRE"), json::array({"NIR", "Red edge"}));
        EXPECT_EQ(category("NDRE"), "multispectral");
        EXPECT_EQ(bands("ENDVI"), json::array({"Green", "Blue", "NIR"}));
        EXPECT_EQ(bands("NDYI"), json::array({"Green", "Blue"}));
        EXPECT_EQ(category("NDYI"), "rgb");
        EXPECT_EQ(bands("CELSIUS"), json::array({"Thermal"}));
        EXPECT_EQ(category("CELSIUS"), "thermal");
        EXPECT_EQ(category("KELVIN"), "thermal");
        // The reason keeps its text
        EXPECT_EQ(byId.at("NDVI")["disabledReason"], "Requires bands: RN");
    }
}
