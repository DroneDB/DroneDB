/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The bands a formula render reads (computeFormulaValues under NodataMask::AnyBand, the rule of the tiles, the
// thumbnails, the exports and the formula statistics): only those that can change its pixels, the bands the band
// filter maps, the first band of a thermal formula, the alpha band and the bands with a NoData value. The values are
// those of a read of every band, which the render did before: the reference below reads every band and masks them
// as it did, and every sample type, band count, NoData value (none, ordinary, NaN, at the limit of the type), alpha
// band, dataset mask, interleaving, overview and window of the matrix gives the same floats.

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

#include "ddb.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "raster_utils.h"
#include "thermal.h"
#include "vegetation.h"
#include "vis_render.h"

namespace
{
    using namespace ddb;

    class FormulaBandReads : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    struct RasterLayout
    {
        GDALDataType type = GDT_Float32;
        int dataBands = 5;
        int width = 37; // not a multiple of the block: partial blocks at the right and the bottom
        int height = 23;
        int block = 16;
        bool pixelInterleaved = false;
        bool alpha = false;       // a last band that is alpha (0 at some pixels)
        bool datasetMask = false; // an internal mask for every band (0 at some pixels)
        bool overviews = false;
        std::vector<std::optional<double>> nodata; // per data band, none when shorter
        bool nanInBandsWithoutNodata = false;      // NaN and infinities in the float bands without NoData
        std::optional<double> scaleOfBand2;        // a scale (and an offset) on the second band
    };

    // The highest and the lowest value of a type, the NoData values at its limits
    double typeMax(GDALDataType type)
    {
        switch (type)
        {
        case GDT_Byte: return 255;
        case GDT_UInt16: return 65535;
        case GDT_Int16: return 32767;
        case GDT_UInt32: return 4294967295.0;
        case GDT_Int32: return 2147483647.0;
        case GDT_Float32: return FLT_MAX;
        default: return DBL_MAX;
        }
    }

    double typeMin(GDALDataType type)
    {
        switch (type)
        {
        case GDT_Byte:
        case GDT_UInt16:
        case GDT_UInt32: return 0;
        case GDT_Int16: return -32768;
        case GDT_Int32: return -2147483648.0;
        case GDT_Float32: return -FLT_MAX;
        default: return -DBL_MAX;
        }
    }

    // A value of band b (1-based) at (x, y) that the type holds: an even spread over the type, signed ones included
    double sampleValue(GDALDataType type, int b, int x, int y)
    {
        const int cell = (x * 7 + y * 13 + b * 29) % 97; // 0..96
        switch (type)
        {
        case GDT_Byte: return cell * 2;
        case GDT_UInt16: return cell * 600;
        case GDT_Int16: return cell * 600 - 25000;
        case GDT_UInt32: return cell * 40000000.0;
        case GDT_Int32: return cell * 40000000.0 - 2000000000.0;
        default: return cell / 97.0 - 0.3;
        }
    }

    fs::path writeRaster(const fs::path &path, const RasterLayout &layout)
    {
        const int bandCount = layout.dataBands + (layout.alpha ? 1 : 0);
        CPLStringList options;
        options.AddString("TILED=YES");
        options.AddString(("BLOCKXSIZE=" + std::to_string(layout.block)).c_str());
        options.AddString(("BLOCKYSIZE=" + std::to_string(layout.block)).c_str());
        options.AddString(layout.pixelInterleaved ? "INTERLEAVE=PIXEL" : "INTERLEAVE=BAND");
        std::unique_ptr<GDALDataset> ds(GetGDALDriverManager()->GetDriverByName("GTiff")->Create(
            path.string().c_str(), layout.width, layout.height, bandCount, layout.type, options.List()));
        EXPECT_NE(ds, nullptr);
        const double gt[6] = {500000.0, 0.5, 0.0, 5000000.0, 0.0, -0.5};
        ds->SetGeoTransform(const_cast<double *>(gt));
        OGRSpatialReference srs;
        srs.importFromEPSG(32633);
        ds->SetSpatialRef(&srs);

        const bool floating = GDALDataTypeIsFloating(layout.type) != 0;
        std::vector<double> values(static_cast<size_t>(layout.width) * layout.height);
        for (int b = 1; b <= layout.dataBands; b++)
        {
            GDALRasterBand *band = ds->GetRasterBand(b);
            // A plain flag and value, not a copied std::optional: GCC 13 at -O3 reports the
            // copy's value as maybe uninitialized (-Wmaybe-uninitialized) where it is read.
            const size_t slot = static_cast<size_t>(b - 1);
            const bool hasNodata = slot < layout.nodata.size() && layout.nodata[slot].has_value();
            const double nodata = hasNodata ? *layout.nodata[slot] : 0.0;
            if (hasNodata)
                band->SetNoDataValue(nodata);
            for (int y = 0; y < layout.height; y++)
                for (int x = 0; x < layout.width; x++)
                {
                    double v = sampleValue(layout.type, b, x, y);
                    if (hasNodata && (x + y * 3 + b) % 11 == 0)
                        v = nodata;
                    else if (!hasNodata && floating && layout.nanInBandsWithoutNodata)
                    {
                        if ((x * y + b) % 7 == 0)
                            v = std::numeric_limits<double>::quiet_NaN();
                        else if ((x + 2 * y + b) % 13 == 1)
                            v = (x % 2 == 0 ? 1 : -1) * std::numeric_limits<double>::infinity();
                    }
                    values[static_cast<size_t>(y) * layout.width + x] = v;
                }
            EXPECT_EQ(band->RasterIO(GF_Write, 0, 0, layout.width, layout.height, values.data(), layout.width,
                                     layout.height, GDT_Float64, 0, 0, nullptr),
                      CE_None);
            if (b == 2 && layout.scaleOfBand2)
            {
                band->SetScale(*layout.scaleOfBand2);
                band->SetOffset(0.25);
            }
        }
        if (layout.alpha)
        {
            GDALRasterBand *alpha = ds->GetRasterBand(bandCount);
            alpha->SetColorInterpretation(GCI_AlphaBand);
            for (int y = 0; y < layout.height; y++)
                for (int x = 0; x < layout.width; x++)
                    values[static_cast<size_t>(y) * layout.width + x] = (x * 5 + y) % 9 == 0 ? 0.0 : 255.0;
            EXPECT_EQ(alpha->RasterIO(GF_Write, 0, 0, layout.width, layout.height, values.data(), layout.width,
                                      layout.height, GDT_Float64, 0, 0, nullptr),
                      CE_None);
        }
        if (layout.datasetMask)
        {
            EXPECT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_None);
            std::vector<uint8_t> mask(values.size());
            for (int y = 0; y < layout.height; y++)
                for (int x = 0; x < layout.width; x++)
                    mask[static_cast<size_t>(y) * layout.width + x] = (x + y * 7) % 10 == 0 ? 0 : 255;
            EXPECT_EQ(ds->GetRasterBand(1)->GetMaskBand()->RasterIO(GF_Write, 0, 0, layout.width, layout.height,
                                                                    mask.data(), layout.width, layout.height,
                                                                    GDT_Byte, 0, 0, nullptr),
                      CE_None);
        }
        if (layout.overviews)
        {
            const int levels[] = {2, 4};
            EXPECT_EQ(ds->BuildOverviews("NEAREST", 2, levels, 0, nullptr, nullptr, nullptr), CE_None);
        }
        return path;
    }

    // The values of the formula from a read of every band of the raster, masked as the render masked them before it
    // read only the bands that decide: the reference of the matrix
    std::vector<float> everyBandValues(GDALDatasetH ds, const FormulaPlan &plan, const RasterWindow &window)
    {
        const size_t pixels = window.outPixels();
        std::vector<std::vector<float>> bands(plan.bandCount, std::vector<float>(pixels));
        std::vector<float *> ptrs;
        for (int b = 0; b < plan.bandCount; b++)
        {
            EXPECT_EQ(GDALRasterIO(GDALGetRasterBand(ds, b + 1), GF_Read, window.x, window.y, window.xsize,
                                   window.ysize, bands[b].data(), window.outWidth, window.outHeight, GDT_Float32, 0,
                                   0),
                      CE_None);
            ptrs.push_back(bands[b].data());
        }
        std::vector<uint8_t> mask;
        if (plan.nodata.datasetMask)
        {
            mask.resize(pixels);
            EXPECT_EQ(GDALRasterIO(GDALGetMaskBand(GDALGetRasterBand(ds, 1)), GF_Read, window.x, window.y,
                                   window.xsize, window.ysize, mask.data(), window.outWidth, window.outHeight,
                                   GDT_Byte, 0, 0),
                      CE_None);
        }
        premaskNodata(ptrs, pixels, plan.bandCount, plan.nodata, NODATA_SENTINEL, mask.empty() ? nullptr : mask.data());
        for (int b = 0; b < plan.bandCount; b++)
        {
            int hasScale = FALSE, hasOffset = FALSE;
            const double scale = GDALGetRasterScale(GDALGetRasterBand(ds, b + 1), &hasScale);
            const double offset = GDALGetRasterOffset(GDALGetRasterBand(ds, b + 1), &hasOffset);
            if (!((hasScale && scale != 1.0) || (hasOffset && offset != 0.0)))
                continue;
            for (float &v : bands[b])
                if (v != NODATA_SENTINEL)
                    v = static_cast<float>(v * (hasScale ? scale : 1.0) + (hasOffset ? offset : 0.0));
        }
        std::vector<float> result(pixels);
        VegetationEngine::instance().applyFormula(*plan.formula, plan.filter, ptrs, result.data(), pixels,
                                                  NODATA_SENTINEL, plan.bandUnit);
        return result;
    }

    // The bands (0-based) that can decide a pixel of the plan, worked out from the rule and not from the engine
    std::set<int> bandsThatDecide(const FormulaPlan &plan)
    {
        std::set<int> bands;
        for (const int b : {plan.filter.R, plan.filter.G, plan.filter.B, plan.filter.N, plan.filter.Re})
            if (b >= 0 && b < plan.bandCount)
                bands.insert(b);
        if (isThermalFormulaId(plan.formula->id))
            bands.insert(0);
        if (plan.nodata.alphaBandIdx >= 0)
            bands.insert(plan.nodata.alphaBandIdx);
        for (int b = 0; b < plan.bandCount; b++)
            if (plan.nodata.bands[b].has)
                bands.insert(b);
        return bands;
    }

    struct FormulaCase
    {
        std::string formula;
        std::string filter;
    };

    std::string describe(const RasterLayout &layout)
    {
        std::string text = std::string(GDALGetDataTypeName(layout.type)) + " " + std::to_string(layout.dataBands) +
                           " bands" + (layout.pixelInterleaved ? " pixel" : " band") + " interleaved";
        if (layout.alpha)
            text += ", alpha";
        if (layout.datasetMask)
            text += ", mask";
        if (layout.overviews)
            text += ", overviews";
        if (layout.nanInBandsWithoutNodata)
            text += ", NaN";
        if (layout.scaleOfBand2)
            text += ", scale";
        for (size_t b = 0; b < layout.nodata.size(); b++)
            if (layout.nodata[b])
                text += ", nodata " + std::to_string(b + 1) + "=" + std::to_string(*layout.nodata[b]);
        return text;
    }

    // Every type, band count and NoData layout of the matrix, four variants each: the bands without NoData (NaN and
    // infinities in the float ones), NoData at the limits of the type on some of the bands with an alpha band, a
    // NoData value on every band (NaN for the float types) with a dataset mask and overviews, and NoData on the
    // first two bands only with a scaled second band
    std::vector<RasterLayout> layoutMatrix()
    {
        std::vector<RasterLayout> layouts;
        for (const GDALDataType type :
             {GDT_Byte, GDT_UInt16, GDT_Int16, GDT_UInt32, GDT_Int32, GDT_Float32, GDT_Float64})
        {
            const bool floating = GDALDataTypeIsFloating(type) != 0;
            for (const int dataBands : {1, 2, 3, 5, 12})
            {
                for (int variant = 0; variant < 4; variant++)
                {
                    RasterLayout layout;
                    layout.type = type;
                    layout.dataBands = dataBands;
                    layout.nodata.assign(dataBands, std::nullopt);
                    switch (variant)
                    {
                    case 0:
                        layout.nanInBandsWithoutNodata = true;
                        break;
                    case 1:
                        layout.pixelInterleaved = true;
                        layout.alpha = true;
                        layout.nanInBandsWithoutNodata = true;
                        for (int b = 0; b < dataBands; b += 3)
                            layout.nodata[b] = b % 2 == 0 ? typeMax(type) : typeMin(type);
                        break;
                    case 2:
                        layout.datasetMask = true;
                        layout.overviews = true;
                        for (int b = 0; b < dataBands; b++)
                            layout.nodata[b] = floating ? std::numeric_limits<double>::quiet_NaN() : 0.0;
                        break;
                    default:
                        layout.pixelInterleaved = true;
                        layout.alpha = true;
                        layout.overviews = true;
                        layout.block = 32;
                        layout.nanInBandsWithoutNodata = true;
                        layout.scaleOfBand2 = 0.5;
                        for (int b = 0; b < std::min(2, dataBands); b++)
                            layout.nodata[b] = typeMin(type) + 1;
                        break;
                    }
                    layouts.push_back(layout);
                }
            }
        }
        return layouts;
    }

    bool sameFloats(const std::vector<float> &a, const std::vector<float> &b)
    {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    }

    // The matrix: the values of every formula that the raster can compute are those of a read of every band, and
    // the bands read are exactly those that decide (the others have a null entry in buffers.ptrs). A formula that
    // the bands of the raster cannot give is refused before anything is read, as before.
    TEST_F(FormulaBandReads, onlyTheBandsThatDecideAreReadWithTheValuesOfAReadOfEveryBand)
    {
        TestArea ta(TEST_NAME, true);
        const std::vector<FormulaCase> cases = {
            {"NDVI", "RGBNRe"}, {"NDVI", "RN"},  {"VARI", "RGB"},     {"EXG", "BGR"},     {"NDRE", "RGBNRe"},
            {"GRVI", "NG"},     {"EVI", "NRB"}, {"CELSIUS", "RGBN"}, {"KELVIN", "T"},    {"ENDVI", "PPGBN"}};
        const std::vector<RasterWindow> windows = {
            {0, 0, 37, 23, 37, 23},  // the whole raster
            {0, 0, 37, 23, 12, 8},   // decimated (from the overviews when there are)
            {5, 3, 29, 17, 29, 17},  // partial blocks on every side
            {36, 22, 1, 1, 1, 1},    // the last pixel, smaller than a block
            {10, 10, 4, 3, 8, 6}};   // magnified

        int computed = 0, refused = 0, unread = 0;
        int index = 0;
        for (const RasterLayout &layout : layoutMatrix())
        {
            const fs::path path = writeRaster(ta.getPath("r" + std::to_string(index++) + ".tif"), layout);
            std::unique_ptr<GDALDataset> ds(
                static_cast<GDALDataset *>(GDALOpen(path.string().c_str(), GA_ReadOnly)));
            ASSERT_NE(ds, nullptr);
            GDALDatasetH handle = GDALDataset::ToHandle(ds.get());
            const int bandCount = GDALGetRasterCount(handle);

            std::vector<std::pair<std::string, BandFilter>> plans;
            for (const FormulaCase &c : cases)
                plans.emplace_back(c.formula, VegetationEngine::parseFilter(c.filter, bandCount));
            // A filter that maps a band the raster does not have (the band is not read, and the formula has no
            // value of it, before as now)
            BandFilter beyond = VegetationEngine::parseFilter("R", bandCount);
            beyond.N = bandCount + 2;
            plans.emplace_back("NDVI", beyond);

            for (const auto &[formula, filter] : plans)
            {
                const std::string what = describe(layout) + ": " + formula + " " + filter.id;
                FormulaPlan plan;
                try
                {
                    plan = planFormula(handle, formula, filter, "", false);
                }
                catch (const RefusedArgumentException &e)
                {
                    // The bands the formula needs (Requires bands), or reflectance for a formula that adds
                    // constants to them
                    const std::string message = e.what();
                    EXPECT_TRUE(message.find("Requires bands: ") == 0 ||
                                message.find("needs reflectance values") != std::string::npos)
                        << what << ": " << message;
                    refused++;
                    continue;
                }
                const std::set<int> deciding = bandsThatDecide(plan);
                for (const RasterWindow &window : windows)
                {
                    FormulaBuffers buffers;
                    computeFormulaValues(handle, plan, window, buffers);
                    EXPECT_TRUE(sameFloats(buffers.result, everyBandValues(handle, plan, window)))
                        << what << ", window " << window.x << "," << window.y << " " << window.xsize << "x"
                        << window.ysize << " -> " << window.outWidth << "x" << window.outHeight;
                    ASSERT_EQ(buffers.ptrs.size(), static_cast<size_t>(bandCount)) << what;
                    for (int b = 0; b < bandCount; b++)
                    {
                        EXPECT_EQ(buffers.ptrs[b] != nullptr, deciding.count(b) == 1) << what << ", band " << b + 1;
                        if (buffers.ptrs[b] == nullptr)
                            unread++;
                    }
                    computed++;
                }
            }
        }
        // The matrix computes and refuses formulas, and leaves bands unread
        EXPECT_GT(computed, 1000);
        EXPECT_GT(refused, 100);
        EXPECT_GT(unread, 1000);
    }

    // A VRT of six bands of a 4 x 3 raster (R, G, B, N and two more), each from its own band of a GeoTIFF, but the
    // bands `missing` (1-based), whose file does not exist: they open, and a read of them fails. `nodata` gives a
    // NoData value to a band, and `alpha` makes the last band the alpha band.
    fs::path vrtWithMissingBands(TestArea &ta, const std::string &name, const std::set<int> &missing,
                                 const std::set<int> &nodata = {}, bool alpha = false)
    {
        const int bands = 6;
        const fs::path source = ta.getPath(name + "-source.tif");
        {
            std::unique_ptr<GDALDataset> ds(GetGDALDriverManager()->GetDriverByName("GTiff")->Create(
                source.string().c_str(), 4, 3, bands, GDT_Float32, nullptr));
            EXPECT_NE(ds, nullptr);
            for (int b = 1; b <= bands; b++)
            {
                std::vector<float> values(12);
                for (int i = 0; i < 12; i++)
                    values[i] = b == bands && alpha ? 255.0f : static_cast<float>(0.1 * b + 0.01 * i);
                EXPECT_EQ(ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 4, 3, values.data(), 4, 3, GDT_Float32, 0,
                                                         0, nullptr),
                          CE_None);
            }
        }
        std::string xml = "<VRTDataset rasterXSize=\"4\" rasterYSize=\"3\">\n"
                          " <SRS>EPSG:3857</SRS>\n <GeoTransform>0, 1, 0, 0, 0, -1</GeoTransform>\n";
        for (int b = 1; b <= bands; b++)
        {
            xml += " <VRTRasterBand dataType=\"Float32\" band=\"" + std::to_string(b) + "\">\n";
            if (nodata.count(b))
                xml += "  <NoDataValue>-1</NoDataValue>\n";
            if (alpha && b == bands)
                xml += "  <ColorInterp>Alpha</ColorInterp>\n";
            xml += "  <SimpleSource><SourceFilename relativeToVRT=\"0\">" +
                   (missing.count(b) ? ta.getPath(name + "-missing.tif") : source).string() +
                   "</SourceFilename><SourceBand>" + std::to_string(b) + "</SourceBand></SimpleSource>\n";
            xml += " </VRTRasterBand>\n";
        }
        xml += "</VRTDataset>\n";
        const fs::path vrt = ta.getPath(name + ".vrt");
        std::ofstream(vrt) << xml;
        return vrt;
    }

    // The values of formula with filter over the whole VRT; throws the GDALException of a band that cannot be read
    std::vector<float> computeOver(const fs::path &vrt, const std::string &formula, const std::string &filter)
    {
        std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(vrt.string().c_str(), GA_ReadOnly)));
        EXPECT_NE(ds, nullptr);
        GDALDatasetH handle = GDALDataset::ToHandle(ds.get());
        const FormulaPlan plan =
            planFormula(handle, formula, VegetationEngine::parseFilter(filter, GDALGetRasterCount(handle)), "", false);
        FormulaBuffers buffers;
        computeFormulaValues(handle, plan, RasterWindow{0, 0, 4, 3, 4, 3}, buffers);
        return buffers.result;
    }

    // A band that cannot change a pixel is not read at all: its read would fail here, and the formula is computed
    // with the values of the raster whose band can be read. The bands that can change a pixel are still read: a
    // band the filter maps that the formula does not use, a band with a NoData value, the first band of a thermal
    // formula and the alpha band.
    TEST_F(FormulaBandReads, aBandThatCannotChangeAPixelIsNotRead)
    {
        TestArea ta(TEST_NAME, true);
        const auto reference = computeOver(vrtWithMissingBands(ta, "whole", {}), "NDVI", "RGBN");

        // Bands 5 and 6: no NoData value, not mapped by RGBN
        EXPECT_TRUE(sameFloats(computeOver(vrtWithMissingBands(ta, "unmapped", {5, 6}), "NDVI", "RGBN"), reference));
        // The same with the alpha band read: band 6 is alpha, band 5 is not read
        EXPECT_NO_THROW(computeOver(vrtWithMissingBands(ta, "alphaRead", {5}, {}, true), "NDVI", "RGBN"));

        // G and B are mapped: NDVI does not use them, but a pixel where they have no value has none
        EXPECT_THROW(computeOver(vrtWithMissingBands(ta, "mapped", {2}), "NDVI", "RGBN"), GDALException);
        // A band with a NoData value leaves its NoData pixels out
        EXPECT_THROW(computeOver(vrtWithMissingBands(ta, "nodata", {5}, {5}), "NDVI", "RGBN"), GDALException);
        // A thermal formula reads the first band whatever the filter maps
        EXPECT_THROW(computeOver(vrtWithMissingBands(ta, "thermal", {1}), "CELSIUS", "PPPN"), GDALException);
        EXPECT_NO_THROW(computeOver(vrtWithMissingBands(ta, "thermalOthers", {2, 3, 5, 6}), "CELSIUS", "PPPN"));
        // The alpha band
        EXPECT_THROW(computeOver(vrtWithMissingBands(ta, "alpha", {6}, {}, true), "NDVI", "RGBN"), GDALException);
    }

    // The bands applyFormula looks at: those the filter maps to R, G, B, N and Re, and the first one for a thermal
    // formula (T is not one of them: the thermal formulas read the first band)
    TEST_F(FormulaBandReads, theInputBandsOfAFormulaAreThoseTheFilterMapsAndTheFirstForAThermalOne)
    {
        const VegetationEngine &engine = VegetationEngine::instance();
        const auto inputs = [&](const std::string &formula, const std::string &filter, int bands) {
            return VegetationEngine::inputBands(*engine.getFormula(formula), VegetationEngine::parseFilter(filter, bands));
        };
        EXPECT_EQ(inputs("NDVI", "RGBNRe", 10), (std::vector<int>{0, 1, 2, 3, 4}));
        EXPECT_EQ(inputs("NDVI", "PRPPN", 10), (std::vector<int>{1, 4}));
        EXPECT_EQ(inputs("NDVI", "RGBNRe", 3), (std::vector<int>{0, 1, 2}));
        EXPECT_EQ(inputs("CELSIUS", "PPN", 3), (std::vector<int>{0, 2}));
        EXPECT_EQ(inputs("KELVIN", "T", 1), (std::vector<int>{0}));
        EXPECT_EQ(inputs("VARI", "PTBGR", 5), (std::vector<int>{2, 3, 4}));
    }
}
