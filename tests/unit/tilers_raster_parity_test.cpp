/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The rasters libddb reads in its process (the COG of a build, the normalized copy of any other
// raster) are read by ddb-tilers (ddbtRasterOpen, ddbtRasterInfo, ddbtRasterRead) as GDAL reads them:
// the same size, type, blocks, overviews, mask, georeferencing and band metadata, and the same
// samples for every window and buffer size (nearest resampling, GDAL's choice of overview).

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "ddbt/ddbt.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "json.h"
#include "mio.h"
#include "raster_stats.h"
#include "vegetation.h"
#include "vis_render.h"
#include "test.h"
#include "testarea.h"
#include "tilers_raster.h"

namespace
{
    using namespace ddb;

    struct Case
    {
        std::string name;
        GDALDataType type = GDT_Byte;
        int bands = 1;
        bool tiled = true;
        bool bandInterleaved = false;
        bool nodata = false;
        bool scaled = false;
        bool internalMask = false;
        bool alpha = false;  // the last band is alpha
        bool overviews = false;
        int epsg = 3857;     // 0: not georeferenced
    };

    double sampleOf(const Case &c, int band, int x, int y)
    {
        if (c.alpha && band == c.bands)
        {
            // UInt16: the values GDAL rescales to a mask, among them 1..256, which stay above 0
            static const double uint16Alpha[5] = {0, 1, 256, 257, 65535};
            return c.type == GDT_UInt16 ? uint16Alpha[(x / 7 + y / 5) % 5] : (x / 7 + y / 5) % 3 == 0 ? 0 : 255;
        }
        const double v = (x * 37 + y * 101 + band * 13) % 251;
        switch (c.type)
        {
        case GDT_Int16:
        case GDT_Int32:
            return v - 120;
        case GDT_Float32:
        case GDT_Float64:
            return v / 7.0 - 11.25;
        default:
            return v;
        }
    }

    fs::path writeRaster(const fs::path &path, const Case &c, int width, int height)
    {
        CPLStringList options;
        options.SetNameValue("COMPRESS", "DEFLATE");
        if (c.tiled)
        {
            options.SetNameValue("TILED", "YES");
            options.SetNameValue("BLOCKXSIZE", "64");
            options.SetNameValue("BLOCKYSIZE", "64");
        }
        else
            options.SetNameValue("BLOCKYSIZE", "7");
        options.SetNameValue("INTERLEAVE", c.bandInterleaved ? "BAND" : "PIXEL");
        if (c.alpha)
            options.SetNameValue("ALPHA", "YES");
        auto ds = adoptDataset(GDALCreate(GDALGetDriverByName("GTiff"), io::toUtf8(path).c_str(), width, height,
                                          c.bands, c.type, options.List()));
        EXPECT_TRUE(ds);
        GDALDatasetH h = datasetHandle(ds);
        if (c.epsg != 0)
        {
            const double gt[6] = {500000.0, 0.5, 0, 4500000.0, 0, -0.5};
            GDALSetGeoTransform(h, const_cast<double *>(gt));
            OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
            OSRImportFromEPSG(srs, c.epsg);
            GDALSetSpatialRef(h, srs);
            OSRDestroySpatialReference(srs);
        }
        std::vector<double> values(static_cast<size_t>(width) * height);
        for (int b = 1; b <= c.bands; b++)
        {
            GDALRasterBandH band = GDALGetRasterBand(h, b);
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                    values[static_cast<size_t>(y) * width + x] = sampleOf(c, b, x, y);
            EXPECT_EQ(GDALRasterIO(band, GF_Write, 0, 0, width, height, values.data(), width, height, GDT_Float64, 0, 0),
                      CE_None);
            if (c.nodata)
                GDALSetRasterNoDataValue(band, sampleOf(c, b, 3, 3));
            if (c.scaled)
            {
                GDALSetRasterScale(band, 0.25);
                GDALSetRasterOffset(band, -3.5);
                GDALSetRasterUnitType(band, "metre");
            }
            GDALSetDescription(band, ("band " + std::to_string(b)).c_str());
        }
        if (c.internalMask)
        {
            EXPECT_EQ(GDALCreateDatasetMaskBand(h, GMF_PER_DATASET), CE_None);
            GDALRasterBandH mask = GDALGetMaskBand(GDALGetRasterBand(h, 1));
            std::vector<uint8_t> m(static_cast<size_t>(width) * height);
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                    m[static_cast<size_t>(y) * width + x] = (x + 2 * y) % 9 == 0 ? 0 : 255;
            EXPECT_EQ(GDALRasterIO(mask, GF_Write, 0, 0, width, height, m.data(), width, height, GDT_Byte, 0, 0), CE_None);
        }
        if (c.overviews)
        {
            int levels[2] = {2, 4};
            EXPECT_EQ(GDALBuildOverviews(h, "NEAREST", 2, levels, 0, nullptr, nullptr, nullptr), CE_None);
        }
        return path;
    }

    const char *maskKindOf(int flags)
    {
        if (flags & GMF_ALPHA)
            return "alpha";
        if (flags & GMF_NODATA)
            return "nodata";
        if (flags & GMF_PER_DATASET)
            return "perDataset";
        return "allValid";
    }

    struct Window
    {
        int x, y, w, h, bufW, bufH;
    };

    TEST(TilersRasterParity, everyInfoAndWindowIsWhatGdalReads)
    {
        TestArea ta(TEST_NAME, true);
        const std::vector<Case> cases = {
            {"byte-rgb", GDT_Byte, 3},
            {"byte-rgba-alpha", GDT_Byte, 4, true, false, false, false, false, true, true},
            {"byte-grey-alpha", GDT_Byte, 2, false, true, false, false, false, true},
            {"uint16-rgba-alpha-overviews", GDT_UInt16, 4, true, false, false, false, false, true, true},
            {"uint16-strips-band", GDT_UInt16, 2, false, true, true},
            {"int16-scaled", GDT_Int16, 1, true, false, true, true},
            {"uint32-mask", GDT_UInt32, 1, true, false, false, false, true},
            {"int32-overviews", GDT_Int32, 2, true, true, false, false, false, false, true},
            {"float32-dem", GDT_Float32, 1, true, false, true, true, false, false, true, 32633},
            {"float64-mask-overviews", GDT_Float64, 3, true, false, false, false, true, false, true},
            {"byte-not-georeferenced", GDT_Byte, 3, false, false, false, false, false, false, false, 0},
            {"float32-ms-6", GDT_Float32, 6, true, true, true, false, false, false, true, 4326},
        };
        const int width = 333, height = 257;
        const std::vector<Window> windows = {
            {0, 0, width, height, width, height},       // whole
            {17, 29, 120, 90, 120, 90},                 // a window at full resolution
            {0, 0, width, height, width / 2, height / 2},  // halved
            {5, 3, 300, 250, 81, 67},                   // reduced 3.7 times: an overview when there is one
            {0, 0, width, height, 20, 15},              // a thumbnail
            {200, 100, 40, 30, 60, 45},                 // magnified
            {width - 1, height - 1, 1, 1, 1, 1},        // the last pixel
        };
        for (const Case &c : cases)
        {
            SCOPED_TRACE(c.name);
            const fs::path file = writeRaster(ta.getPath(c.name + ".tif"), c, width, height);
            const auto ds = adoptDataset(GDALOpen(io::toUtf8(file).c_str(), GA_ReadOnly));
            ASSERT_TRUE(ds);
            GDALDatasetH h = datasetHandle(ds);

            const auto tilersRaster = tilers::Raster::open(file);
            ddbtRaster *raster = nullptr;
            ASSERT_EQ(ddbtRasterOpen(io::toUtf8(file).c_str(), &raster), DDBT_ERR_NONE) << ddbtLastError();
            char *infoText = nullptr;
            ASSERT_EQ(ddbtRasterInfo(raster, &infoText), DDBT_ERR_NONE) << ddbtLastError();
            const json info = json::parse(infoText);
            ddbtFree(infoText);

            // The file
            EXPECT_EQ(info.at("width").get<int>(), GDALGetRasterXSize(h));
            EXPECT_EQ(info.at("height").get<int>(), GDALGetRasterYSize(h));
            GDALRasterBandH band1 = GDALGetRasterBand(h, 1);
            EXPECT_EQ(info.at("type").get<std::string>(), GDALGetDataTypeName(GDALGetRasterDataType(band1)));
            int bx = 0, by = 0;
            GDALGetBlockSize(band1, &bx, &by);
            EXPECT_EQ(info.at("blockWidth").get<int>(), bx);
            EXPECT_EQ(info.at("blockHeight").get<int>(), by);
            EXPECT_EQ(tilersRaster->blockWidth(), bx);
            EXPECT_EQ(tilersRaster->blockHeight(), by);
            ASSERT_EQ(static_cast<int>(info.at("overviews").size()), GDALGetOverviewCount(band1));
            for (int o = 0; o < GDALGetOverviewCount(band1); o++)
            {
                GDALRasterBandH ov = GDALGetOverview(band1, o);
                EXPECT_EQ(info.at("overviews")[o][0].get<int>(), GDALGetRasterBandXSize(ov));
                EXPECT_EQ(info.at("overviews")[o][1].get<int>(), GDALGetRasterBandYSize(ov));
            }
            EXPECT_EQ(info.at("mask").get<std::string>(), maskKindOf(GDALGetMaskFlags(band1)));
            double gt[6];
            if (c.epsg != 0)
            {
                ASSERT_EQ(GDALGetGeoTransform(h, gt), CE_None);
                ASSERT_TRUE(info.contains("geoTransform"));
                for (int i = 0; i < 6; i++)
                    EXPECT_EQ(info.at("geoTransform")[i].get<double>(), gt[i]);
                ASSERT_TRUE(info.contains("wkt"));
                const SrsPtr gdalSrs = srsFromWkt(GDALGetProjectionRef(h));
                const SrsPtr ddbtSrs = srsFromWkt(info.at("wkt").get<std::string>().c_str());
                ASSERT_TRUE(gdalSrs && ddbtSrs);
                EXPECT_TRUE(OSRIsSame(gdalSrs.get(), ddbtSrs.get())) << info.at("wkt");
                // The text too: libddb returns it (the CRS of a volume)
                EXPECT_EQ(info.at("wkt").get<std::string>(), std::string(GDALGetProjectionRef(h)));
            }
            else
            {
                EXPECT_FALSE(info.contains("geoTransform"));
                EXPECT_FALSE(info.contains("wkt"));
            }

            // The bands
            ASSERT_EQ(static_cast<int>(info.at("bands").size()), GDALGetRasterCount(h));
            for (int b = 1; b <= GDALGetRasterCount(h); b++)
            {
                SCOPED_TRACE(b);
                GDALRasterBandH band = GDALGetRasterBand(h, b);
                const json &meta = info.at("bands")[b - 1];
                EXPECT_EQ(meta.at("colorInterp").get<std::string>(),
                          GDALGetColorInterpretationName(GDALGetRasterColorInterpretation(band)));
                int has = 0;
                const double nodata = GDALGetRasterNoDataValue(band, &has);
                EXPECT_EQ(meta.contains("nodata"), has != 0);
                if (has)
                {
                    EXPECT_EQ(meta.at("nodata").get<double>(), nodata);
                }
                const double scale = GDALGetRasterScale(band, &has);
                EXPECT_EQ(meta.contains("scale"), has != 0);
                if (has)
                {
                    EXPECT_EQ(meta.at("scale").get<double>(), scale);
                }
                const double offset = GDALGetRasterOffset(band, &has);
                EXPECT_EQ(meta.contains("offset"), has != 0);
                if (has)
                {
                    EXPECT_EQ(meta.at("offset").get<double>(), offset);
                }
                EXPECT_EQ(meta.value("unit", std::string()), std::string(GDALGetRasterUnitType(band)));
                EXPECT_EQ(meta.value("description", std::string()), std::string(GDALGetDescription(band)));
            }

            // The samples of every window, every band, and the dataset mask
            std::vector<int> bands;
            for (int b = 1; b <= GDALGetRasterCount(h); b++)
                bands.push_back(b);
            for (const Window &w : windows)
            {
                SCOPED_TRACE(std::to_string(w.x) + "," + std::to_string(w.y) + " " + std::to_string(w.w) + "x" +
                             std::to_string(w.h) + " -> " + std::to_string(w.bufW) + "x" + std::to_string(w.bufH));
                const size_t plane = static_cast<size_t>(w.bufW) * w.bufH;
                std::vector<double> got(plane * bands.size()), want(plane * bands.size());
                std::vector<uint8_t> gotMask(plane), wantMask(plane, 255);
                ASSERT_EQ(ddbtRasterRead(raster, w.x, w.y, w.w, w.h, w.bufW, w.bufH, bands.data(),
                                         static_cast<int>(bands.size()), DDBT_SAMPLE_FLOAT64, got.data(), gotMask.data()),
                          DDBT_ERR_NONE)
                    << ddbtLastError();
                ASSERT_EQ(GDALDatasetRasterIO(h, GF_Read, w.x, w.y, w.w, w.h, want.data(), w.bufW, w.bufH, GDT_Float64,
                                              static_cast<int>(bands.size()), bands.data(), 0, 0, 0),
                          CE_None);
                size_t differ = 0;
                for (size_t i = 0; i < got.size(); i++)
                    differ += !(got[i] == want[i] || (std::isnan(got[i]) && std::isnan(want[i])));
                EXPECT_EQ(differ, 0u) << "samples that differ from GDAL's";
                if (GDALGetMaskFlags(band1) == GMF_PER_DATASET)
                {
                    ASSERT_EQ(GDALRasterIO(GDALGetMaskBand(band1), GF_Read, w.x, w.y, w.w, w.h, wantMask.data(), w.bufW,
                                           w.bufH, GDT_Byte, 0, 0),
                              CE_None);
                }
                EXPECT_EQ(gotMask, wantMask);

                // Each band with its own mask (the alpha band is that of the others), as GDAL's mask band reads
                for (int b = 1; b <= GDALGetRasterCount(h); b++)
                {
                    SCOPED_TRACE("band " + std::to_string(b));
                    GDALRasterBandH band = GDALGetRasterBand(h, b);
                    const int flags = GDALGetMaskFlags(band);
                    const bool masked =
                        (flags & (GMF_ALPHA | GMF_PER_DATASET)) != 0 && (flags & (GMF_NODATA | GMF_ALL_VALID)) == 0;
                    ASSERT_EQ(tilersRaster->hasMask(b), masked);
                    std::vector<double> samples(plane), bandWant(plane);
                    std::vector<uint8_t> bandMask(plane, 77), bandMaskWant(plane, 77);
                    tilersRaster->readBand(b, w.x, w.y, w.w, w.h, w.bufW, w.bufH, samples.data(), bandMask.data());
                    ASSERT_EQ(GDALRasterIO(band, GF_Read, w.x, w.y, w.w, w.h, bandWant.data(), w.bufW, w.bufH,
                                           GDT_Float64, 0, 0),
                              CE_None);
                    if (masked)
                    {
                        ASSERT_EQ(GDALRasterIO(GDALGetMaskBand(band), GF_Read, w.x, w.y, w.w, w.h, bandMaskWant.data(),
                                               w.bufW, w.bufH, GDT_Byte, 0, 0),
                                  CE_None);
                    }
                    EXPECT_EQ(samples, bandWant);
                    EXPECT_EQ(bandMask, bandMaskWant) << "the mask of the band (untouched without one)";
                }
            }
            ddbtRasterClose(raster);
        }
    }

    // The statistics a pass reads through ddb-tilers (the host) are those it reads through GDAL (the raster
    // helper), to the bit: the exact pass, the overview and the block lattice of a bounded sample, the plan of
    // the sample, with the NoData value of the band and an override
    std::vector<std::optional<BandStatistics>> statisticsOf(const std::function<std::vector<BandStatistics>()> &compute,
                                                            size_t bands)
    {
        try
        {
            const auto stats = compute();
            return {stats.begin(), stats.end()};
        }
        catch (const NoValidPixelsException &)
        {
            return std::vector<std::optional<BandStatistics>>(bands);
        }
    }

    void expectSameStatistics(const std::vector<std::optional<BandStatistics>> &got,
                              const std::vector<std::optional<BandStatistics>> &want)
    {
        ASSERT_EQ(got.size(), want.size());
        for (size_t i = 0; i < got.size(); i++)
        {
            ASSERT_EQ(got[i].has_value(), want[i].has_value()) << "band " << i;
            if (!got[i])
                continue;
            EXPECT_EQ(got[i]->min, want[i]->min) << "band " << i;
            EXPECT_EQ(got[i]->max, want[i]->max) << "band " << i;
            EXPECT_EQ(got[i]->mean, want[i]->mean) << "band " << i;
            EXPECT_EQ(got[i]->stdDev, want[i]->stdDev) << "band " << i;
            EXPECT_EQ(got[i]->validCount, want[i]->validCount) << "band " << i;
            EXPECT_EQ(got[i]->pixelCount, want[i]->pixelCount) << "band " << i;
            EXPECT_EQ(got[i]->sampled, want[i]->sampled) << "band " << i;
        }
    }

    TEST(TilersRasterParity, theStatisticsOfEveryPassAreWhatGdalReads)
    {
        TestArea ta(TEST_NAME, true);
        const std::vector<Case> cases = {
            {"byte-rgba-alpha", GDT_Byte, 4, true, false, false, false, false, true, true},
            {"uint16-rgba-alpha-overviews", GDT_UInt16, 4, true, false, false, false, false, true, true},
            {"uint16-strips-band", GDT_UInt16, 2, false, true, true},
            {"int16-scaled", GDT_Int16, 1, true, false, true, true},
            {"uint32-mask", GDT_UInt32, 1, true, false, false, false, true},
            {"float32-dem", GDT_Float32, 1, true, false, true, true, false, false, true, 32633},
            {"float64-mask-overviews", GDT_Float64, 3, true, false, false, false, true, false, true},
            {"float32-ms-6", GDT_Float32, 6, true, true, true, false, false, false, true, 4326},
        };
        const int width = 333, height = 257;
        for (const Case &c : cases)
        {
            SCOPED_TRACE(c.name);
            const fs::path file = writeRaster(ta.getPath(c.name + ".tif"), c, width, height);
            const auto ds = adoptDataset(GDALOpen(io::toUtf8(file).c_str(), GA_ReadOnly));
            ASSERT_TRUE(ds);
            const auto raster = tilers::Raster::open(file);
            const auto gdal = rasterSourceOf(datasetHandle(ds));
            const auto tilers = rasterSourceOf(*raster);
            std::vector<int> bands;
            for (int b = 1; b <= c.bands; b++)
                bands.push_back(b);

            for (const std::optional<double> nodata : {std::optional<double>(), std::optional<double>(sampleOf(c, 1, 5, 5))})
            {
                SCOPED_TRACE(nodata ? "override" : "the NoData of the band");
                expectSameStatistics(statisticsOf([&] { return computeBandStatistics(*tilers, bands, nodata); }, bands.size()),
                                     statisticsOf([&] { return computeBandStatistics(*gdal, bands, nodata); }, bands.size()));
                // A budget that an overview holds (the variants with overviews), and one that only whole blocks
                // on a lattice fit in
                for (const uint64_t budget : {uint64_t{30000}, uint64_t{9000}})
                {
                    SCOPED_TRACE("budget " + std::to_string(budget));
                    const RasterWindow whole{0, 0, width, height, width, height};
                    const BoundedSample got = planBoundedSample(*tilers, bands, whole, budget);
                    const BoundedSample want = planBoundedSample(*gdal, bands, whole, budget);
                    EXPECT_EQ(static_cast<int>(got.kind), static_cast<int>(want.kind));
                    EXPECT_EQ(got.overview, want.overview);
                    EXPECT_EQ(got.outWidth, want.outWidth);
                    EXPECT_EQ(got.outHeight, want.outHeight);
                    EXPECT_EQ(got.blockWidth, want.blockWidth);
                    EXPECT_EQ(got.blockHeight, want.blockHeight);
                    EXPECT_EQ(got.step, want.step);
                    expectSameStatistics(
                        statisticsOf([&] { return sampleBandStatistics(*tilers, bands, nodata, budget); }, bands.size()),
                        statisticsOf([&] { return sampleBandStatistics(*gdal, bands, nodata, budget); }, bands.size()));
                }
            }
        }
    }

    // The values of a formula and of a band read through ddb-tilers (the host) are those read through GDAL (the
    // raster helper, the renders): the NoData of every band, the alpha band or the dataset mask, the data kind,
    // and the values of every window, at full resolution and reduced, under both NoData rules
    TEST(TilersRasterParity, theFormulaAndBandValuesAreWhatGdalReads)
    {
        TestArea ta(TEST_NAME, true);
        struct FormulaCase
        {
            Case raster;
            std::string filter;
            std::string formula;
        };
        const std::vector<FormulaCase> cases = {
            {{"byte-rgba-alpha", GDT_Byte, 4, true, false, false, false, false, true, true}, "RGB", "VARI"},
            {{"uint16-rgba-alpha-overviews", GDT_UInt16, 4, true, false, false, false, false, true, true}, "RGB", "VARI"},
            {{"uint16-mask", GDT_UInt16, 3, true, false, true, false, true}, "RGB", "VARI"},
            {{"float32-ms-5", GDT_Float32, 5, true, true, true, true, false, false, true, 4326}, "RGBReN", "NDVI"},
            {{"float64-mask-ms", GDT_Float64, 5, true, false, false, false, true, false, true}, "RGBReN", "NDRE"},
        };
        const int width = 333, height = 257;
        const std::vector<Window> windows = {
            {0, 0, width, height, width, height},
            {17, 29, 120, 90, 120, 90},
            {0, 0, width, height, 81, 67},
            {200, 100, 40, 30, 60, 45},
        };
        for (const FormulaCase &fc : cases)
        {
            SCOPED_TRACE(fc.raster.name);
            const fs::path file = writeRaster(ta.getPath(fc.raster.name + ".tif"), fc.raster, width, height);
            const auto ds = adoptDataset(GDALOpen(io::toUtf8(file).c_str(), GA_ReadOnly));
            ASSERT_TRUE(ds);
            const auto raster = tilers::Raster::open(file);
            const auto gdal = rasterSourceOf(datasetHandle(ds));
            const auto tilers = rasterSourceOf(*raster);

            const RasterBandNodata want = detectBandNodata(*gdal, fc.raster.bands);
            const RasterBandNodata got = detectBandNodata(*tilers, fc.raster.bands);
            EXPECT_EQ(got.alphaBandIdx, want.alphaBandIdx);
            EXPECT_EQ(got.datasetMask, want.datasetMask);
            ASSERT_EQ(got.bands.size(), want.bands.size());
            for (size_t b = 0; b < got.bands.size(); b++)
            {
                EXPECT_EQ(got.bands[b].has, want.bands[b].has) << "band " << b + 1;
                EXPECT_EQ(got.bands[b].value, want.bands[b].value) << "band " << b + 1;
            }
            const RasterDataKind kindGot = rasterDataKind(*tilers), kindWant = rasterDataKind(*gdal);
            EXPECT_EQ(kindGot.bandCount, kindWant.bandCount);
            EXPECT_EQ(kindGot.floatingPoint, kindWant.floatingPoint);
            EXPECT_EQ(kindGot.typeName, kindWant.typeName);

            const BandFilter filter = VegetationEngine::parseFilter(fc.filter, fc.raster.bands);
            const FormulaPlan planGot = planFormula(*tilers, fc.formula, filter, "", false);
            const FormulaPlan planWant = planFormula(*gdal, fc.formula, filter, "", false);
            EXPECT_EQ(static_cast<int>(planGot.bandUnit), static_cast<int>(planWant.bandUnit));
            for (const Window &w : windows)
            {
                SCOPED_TRACE(std::to_string(w.x) + "," + std::to_string(w.y) + " " + std::to_string(w.w) + "x" +
                             std::to_string(w.h) + " -> " + std::to_string(w.bufW) + "x" + std::to_string(w.bufH));
                const RasterWindow window{w.x, w.y, w.w, w.h, w.bufW, w.bufH};
                for (const NodataMask mask : {NodataMask::AnyBand, NodataMask::BandsRead})
                {
                    FormulaBuffers bufGot, bufWant;
                    computeFormulaValues(*tilers, planGot, window, bufGot, mask);
                    computeFormulaValues(*gdal, planWant, window, bufWant, mask);
                    ASSERT_EQ(bufGot.result.size(), bufWant.result.size());
                    EXPECT_EQ(0, std::memcmp(bufGot.result.data(), bufWant.result.data(),
                                             bufGot.result.size() * sizeof(float)))
                        << "the values of " << fc.formula;
                }
                for (int b = 1; b <= fc.raster.bands; b++)
                {
                    FormulaBuffers bufGot, bufWant;
                    computeBandValues(*tilers, got, b, window, bufGot);
                    computeBandValues(*gdal, want, b, window, bufWant);
                    ASSERT_EQ(bufGot.result.size(), bufWant.result.size());
                    EXPECT_EQ(0, std::memcmp(bufGot.result.data(), bufWant.result.data(),
                                             bufGot.result.size() * sizeof(float)))
                        << "the values of band " << b;
                }
            }
        }
    }
}
