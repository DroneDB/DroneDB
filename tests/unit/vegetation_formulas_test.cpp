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
}
