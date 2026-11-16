/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The renders of a region of a raster (gdalwarp -of MEM -t_srs -te -te_srs -ts -r [-b] [-dstalpha]
// [-dstnodata]) are warped by ddb-tilers (ddbtWarpRaster) as GDALWarp warps them: the same size, bands,
// type, nodata, colour interpretations and samples, bit for bit, from the overview gdalwarp picks.

#include <cmath>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "ddbt/ddbt.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "gdal_utils.h"
#include "json.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    struct Source
    {
        std::string name;
        GDALDataType type = GDT_Byte;
        std::vector<GDALColorInterp> interps;
        int epsg = 32633;
        int width = 400, height = 300;
        double pixel = 0.5;
        std::optional<double> nodata;
        bool mask = false;
        bool overviews = false;
        bool palette = false;  // a colour table on the band
    };

    double sampleOf(const Source &s, int band, int x, int y)
    {
        if (s.interps[static_cast<size_t>(band - 1)] == GCI_AlphaBand)
            return (x / 9 + y / 7) % 4 == 0 ? 0 : 255;
        if (s.nodata && (x * 5 + y * 3) % 17 == 0)
            return *s.nodata;
        const double v = std::fmod(x * 1.7 + y * 2.3 + band * 31.0, 251.0);
        switch (s.type)
        {
        case GDT_Float32:
        case GDT_Float64:
            // Values that fill the mantissa: the float rounding of an interpolation shows any
            // difference in it
            return v * 3.25 - 40.0 + std::sin(x * 0.37 + y * 0.11) * 0.0137;
        case GDT_Int16:
            return std::floor(v * 3) - 300;
        case GDT_UInt16:
            return std::floor(v * 200);
        default:
            return std::floor(v);
        }
    }

    fs::path writeSource(const fs::path &path, const Source &s)
    {
        CPLStringList options;
        options.SetNameValue("TILED", "YES");
        options.SetNameValue("COMPRESS", "DEFLATE");
        const int bands = static_cast<int>(s.interps.size());
        auto ds = adoptDataset(GDALCreate(GDALGetDriverByName("GTiff"), io::toUtf8(path).c_str(), s.width, s.height,
                                          bands, s.type, options.List()));
        EXPECT_TRUE(ds);
        GDALDatasetH h = datasetHandle(ds);
        const double gt[6] = {500000.0, s.pixel, 0, 5000000.0, 0, -s.pixel};
        GDALSetGeoTransform(h, const_cast<double *>(gt));
        OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(srs, s.epsg);
        GDALSetSpatialRef(h, srs);
        OSRDestroySpatialReference(srs);
        std::vector<double> plane(static_cast<size_t>(s.width) * s.height);
        for (int b = 1; b <= bands; b++)
        {
            GDALRasterBandH band = GDALGetRasterBand(h, b);
            GDALSetRasterColorInterpretation(band, s.interps[static_cast<size_t>(b - 1)]);
            for (int y = 0; y < s.height; y++)
                for (int x = 0; x < s.width; x++)
                    plane[static_cast<size_t>(y) * s.width + x] = sampleOf(s, b, x, y);
            EXPECT_EQ(GDALRasterIO(band, GF_Write, 0, 0, s.width, s.height, plane.data(), s.width, s.height,
                                   GDT_Float64, 0, 0),
                      CE_None);
            if (s.nodata)
                GDALSetRasterNoDataValue(band, *s.nodata);
        }
        if (s.mask)
        {
            EXPECT_EQ(GDALCreateDatasetMaskBand(h, GMF_PER_DATASET), CE_None);
            std::vector<uint8_t> m(static_cast<size_t>(s.width) * s.height);
            for (int y = 0; y < s.height; y++)
                for (int x = 0; x < s.width; x++)
                    m[static_cast<size_t>(y) * s.width + x] = (x + y) % 13 < 3 ? 0 : 255;
            EXPECT_EQ(GDALRasterIO(GDALGetMaskBand(GDALGetRasterBand(h, 1)), GF_Write, 0, 0, s.width, s.height,
                                   m.data(), s.width, s.height, GDT_Byte, 0, 0),
                      CE_None);
        }
        if (s.palette)
        {
            // 251 colours, the last one transparent where the nodata value is
            GDALColorTableH table = GDALCreateColorTable(GPI_RGB);
            for (int i = 0; i < 251; i++)
            {
                const GDALColorEntry e = {static_cast<short>(i), static_cast<short>((i * 7) % 256),
                                          static_cast<short>((i * 13) % 256), 255};
                GDALSetColorEntry(table, i, &e);
            }
            EXPECT_EQ(GDALSetRasterColorTable(GDALGetRasterBand(h, 1), table), CE_None);
            GDALDestroyColorTable(table);
        }
        if (s.overviews)
        {
            int levels[3] = {2, 4, 8};
            EXPECT_EQ(GDALBuildOverviews(h, "AVERAGE", 3, levels, 0, nullptr, nullptr, nullptr), CE_None);
        }
        return path;
    }

    struct Render
    {
        std::string name;
        std::string crs;                  // -t_srs and -te_srs
        std::array<double, 4> extent{};   // -te
        int width = 0, height = 0;        // -ts
        bool nearest = false;
        std::vector<int> bands;
        bool dstAlpha = false;
        std::optional<double> dstNodata;
        std::string extentCrs;            // -te_srs when it is not crs
        std::string outputType;           // -ot
        bool noSrcAlpha = false;          // -nosrcalpha
        bool setci = false;               // -setci

        Render withNearest() const { Render r = *this; r.nearest = true; return r; }
        // The warp of the index layer: Float32, the alpha band as data, the bands named
        Render asIndexWarp() const
        {
            Render r = *this;
            r.outputType = "Float32";
            r.noSrcAlpha = true;
            r.setci = true;
            return r;
        }
        Render withBands(std::vector<int> b) const { Render r = *this; r.bands = std::move(b); return r; }
        Render withDstAlpha() const { Render r = *this; r.dstAlpha = true; return r; }
        Render withDstNodata(double v) const { Render r = *this; r.dstNodata = v; return r; }
        Render withExtentCrs(std::string c) const { Render r = *this; r.extentCrs = std::move(c); return r; }
    };

    // A bilinear render of every band of `extent` (in `crs`) on width x height pixels in `crs`
    Render render(std::string name, std::string crs, const std::array<double, 4> &extent, int width, int height)
    {
        Render r;
        r.name = std::move(name);
        r.crs = std::move(crs);
        r.extent = extent;
        r.width = width;
        r.height = height;
        return r;
    }

    // The extent of the source in `crs`, shrunk or grown by `scale` around its centre
    std::array<double, 4> extentOf(const fs::path &file, const std::string &crs, double scale, double shiftX = 0,
                                   double shiftY = 0)
    {
        const auto ds = adoptDataset(GDALOpen(io::toUtf8(file).c_str(), GA_ReadOnly));
        double gt[6];
        GDALGetGeoTransform(datasetHandle(ds), gt);
        const double w = GDALGetRasterXSize(datasetHandle(ds)), h = GDALGetRasterYSize(datasetHandle(ds));
        OGRSpatialReference src, dst;
        src.SetFromUserInput(GDALGetProjectionRef(datasetHandle(ds)));
        dst.SetFromUserInput(crs.c_str());
        src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        dst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        auto ct = std::unique_ptr<OGRCoordinateTransformation>(OGRCreateCoordinateTransformation(&src, &dst));
        double xs[4] = {gt[0], gt[0] + w * gt[1], gt[0], gt[0] + w * gt[1]};
        double ys[4] = {gt[3], gt[3], gt[3] + h * gt[5], gt[3] + h * gt[5]};
        ct->Transform(4, xs, ys);
        const double minX = *std::min_element(xs, xs + 4), maxX = *std::max_element(xs, xs + 4);
        const double minY = *std::min_element(ys, ys + 4), maxY = *std::max_element(ys, ys + 4);
        const double cx = (minX + maxX) / 2 + shiftX * (maxX - minX), cy = (minY + maxY) / 2 + shiftY * (maxY - minY);
        const double hw = (maxX - minX) / 2 * scale, hh = (maxY - minY) / 2 * scale;
        return {cx - hw, cy - hh, cx + hw, cy + hh};
    }

    std::string number(double v)
    {
        char text[64];
        std::snprintf(text, sizeof text, "%.17g", v);
        return text;
    }

    // The render of the file by ddbtWarpRaster, compared with GDALWarp's: size, bands, type, colour
    // interpretations, nodata and every sample
    void expectWarpedAsGdalWarp(const fs::path &file, const Render &r)
    {
        SCOPED_TRACE(r.name);
        // GDAL
        std::vector<std::string> args = {"-of", "MEM", "-t_srs", r.crs, "-te", number(r.extent[0]),
                                         number(r.extent[1]), number(r.extent[2]), number(r.extent[3]),
                                         "-te_srs", r.extentCrs.empty() ? r.crs : r.extentCrs, "-ts", std::to_string(r.width),
                                         std::to_string(r.height), "-r", r.nearest ? "near" : "bilinear"};
        for (const int b : r.bands)
        {
            args.push_back("-b");
            args.push_back(std::to_string(b));
        }
        if (r.dstAlpha)
            args.push_back("-dstalpha");
        if (r.dstNodata)
        {
            args.push_back("-dstnodata");
            args.push_back(number(*r.dstNodata));
        }
        if (!r.outputType.empty())
        {
            args.push_back("-ot");
            args.push_back(r.outputType);
        }
        if (r.noSrcAlpha)
            args.push_back("-nosrcalpha");
        if (r.setci)
            args.push_back("-setci");
        if (r.dstAlpha && !r.outputType.empty())
        {
            args.push_back("-wo");
            args.push_back("DST_ALPHA_MAX=255");
        }
        CPLStringList argv;
        for (const auto &a : args)
            argv.AddString(a.c_str());
        GDALWarpAppOptions *opts = GDALWarpAppOptionsNew(argv.List(), nullptr);
        const auto src = adoptDataset(GDALOpen(io::toUtf8(file).c_str(), GA_ReadOnly));
        GDALDatasetH hSrc = datasetHandle(src);
        int usage = 0;
        const auto want = adoptDataset(GDALWarp("", nullptr, 1, &hSrc, opts, &usage));
        GDALWarpAppOptionsFree(opts);
        ASSERT_TRUE(want);
        GDALDatasetH w = datasetHandle(want);

        // ddb-tilers
        json options = {{"targetCrs", r.crs}, {"extent", r.extent},
                        {"extentCrs", r.extentCrs.empty() ? r.crs : r.extentCrs},
                        {"width", r.width}, {"height", r.height},
                        {"resampling", r.nearest ? "nearest" : "bilinear"}};
        if (!r.bands.empty())
            options["bands"] = r.bands;
        if (r.dstAlpha)
            options["dstAlpha"] = true;
        if (r.dstNodata)
            options["dstNodata"] = *r.dstNodata;
        if (!r.outputType.empty())
            options["outputType"] = r.outputType;
        if (r.noSrcAlpha)
            options["noSrcAlpha"] = true;
        if (r.setci)
            options["setColorInterp"] = true;
        char *infoText = nullptr;
        uint8_t *pixels = nullptr;
        size_t size = 0;
        ASSERT_EQ(ddbtWarpRaster(io::toUtf8(file).c_str(), options.dump().c_str(), &infoText, &pixels, &size),
                  DDBT_ERR_NONE)
            << ddbtLastError();
        const json info = json::parse(infoText);
        ddbtFree(infoText);

        ASSERT_EQ(info.at("width").get<int>(), GDALGetRasterXSize(w));
        ASSERT_EQ(info.at("height").get<int>(), GDALGetRasterYSize(w));
        ASSERT_EQ(info.at("bands").get<int>(), GDALGetRasterCount(w));
        const GDALDataType type = GDALGetRasterDataType(GDALGetRasterBand(w, 1));
        EXPECT_EQ(info.at("type").get<std::string>(), GDALGetDataTypeName(type));
        const size_t plane = static_cast<size_t>(r.width) * r.height * GDALGetDataTypeSizeBytes(type);
        for (int b = 1; b <= GDALGetRasterCount(w); b++)
        {
            SCOPED_TRACE("band " + std::to_string(b));
            GDALRasterBandH band = GDALGetRasterBand(w, b);
            EXPECT_EQ(info.at("colorInterp")[b - 1].get<std::string>(),
                      GDALGetColorInterpretationName(GDALGetRasterColorInterpretation(band)));
            int has = 0;
            const double nd = GDALGetRasterNoDataValue(band, &has);
            // The nodata of the output is that of every band but the alpha band
            const bool alphaBand = b == info.at("alphaBand").get<int>();
            EXPECT_EQ(info.contains("nodata") && !alphaBand, has != 0);
            if (has && info.contains("nodata") && !alphaBand)
            {
                EXPECT_EQ(info.at("nodata").get<double>(), nd);
            }
            std::vector<uint8_t> expected(plane);
            ASSERT_EQ(GDALRasterIO(band, GF_Read, 0, 0, r.width, r.height, expected.data(), r.width, r.height,
                                   type, 0, 0),
                      CE_None);
            size_t differ = 0, first = 0;
            const size_t ss = static_cast<size_t>(GDALGetDataTypeSizeBytes(type));
            const uint8_t *got = pixels + static_cast<size_t>(b - 1) * plane;
            for (size_t i = 0; i < plane; i += ss)
            {
                if (std::memcmp(expected.data() + i, got + i, ss) == 0)
                    continue;
                if (differ++ == 0)
                    first = i / ss;
            }
            // The first sample that differs, as doubles
            const auto valueAt = [&](const uint8_t *data) {
                double v = 0;
                GDALCopyWords(data + first * ss, type, 0, &v, GDT_Float64, 0, 1);
                return v;
            };
            // How this build of GDAL makes a half of a double, which depends on the compiler (_Float16 rounds,
            // its absence truncates): what a difference of halves comes from
            const auto gdalHalfOf = [](double v) {
                uint16_t h = 0;
                GDALCopyWords(&v, GDT_Float64, 0, &h, GDT_Float16, 0, 1);
                double back = 0;
                GDALCopyWords(&h, GDT_Float16, 0, &back, GDT_Float64, 0, 1);
                return back;
            };
            EXPECT_EQ(differ, 0u) << "samples that differ from GDALWarp's (level " << info.at("sourceLevel").get<int>()
                                  << "), the first at (" << first % static_cast<size_t>(r.width) << ", "
                                  << first / static_cast<size_t>(r.width) << "): " << valueAt(got) << " for "
                                  << valueAt(expected.data())
                                  << (type == GDT_Float16 ? " (GDAL makes " + std::to_string(gdalHalfOf(-9999.0)) +
                                                                " of -9999 in this build)"
                                                          : std::string());
        }
        ddbtFree(pixels);
        // The colour table gdalwarp copies from the first band of the source
        const GDALColorTableH table = GDALGetRasterColorTable(GDALGetRasterBand(w, 1));
        ASSERT_EQ(info.contains("colorTable"), table != nullptr);
        if (table != nullptr)
        {
            const auto &got = info.at("colorTable");
            ASSERT_EQ(static_cast<int>(got.size()), GDALGetColorEntryCount(table));
            for (int i = 0; i < GDALGetColorEntryCount(table); i++)
            {
                const GDALColorEntry *e = GDALGetColorEntry(table, i);
                EXPECT_EQ(got[static_cast<size_t>(i)], json::array({e->c1, e->c2, e->c3, e->c4})) << "entry " << i;
            }
        }
    }

    TEST(TilersWarpParity, aRegionIsWarpedAsGdalWarpWarpsIt)
    {
        TestArea ta(TEST_NAME, true);
        // name, type, colour interpretations, EPSG, size, pixel, nodata, internal mask, overviews, palette
        const std::vector<Source> sources = {
            {"rgb", GDT_Byte, {GCI_RedBand, GCI_GreenBand, GCI_BlueBand}, 32633, 400, 300, 0.5, std::nullopt, false,
             false, false},
            {"rgba", GDT_Byte, {GCI_RedBand, GCI_GreenBand, GCI_BlueBand, GCI_AlphaBand}, 32633, 400, 300, 0.5,
             std::nullopt, false, false, false},
            {"dem", GDT_Float32, {GCI_GrayIndex}, 32633, 400, 300, 0.5, -9999.0, false, false, false},
            {"masked", GDT_Byte, {GCI_RedBand, GCI_GreenBand, GCI_BlueBand}, 32633, 400, 300, 0.5, std::nullopt, true,
             false, false},
            {"big", GDT_Byte, {GCI_RedBand, GCI_GreenBand, GCI_BlueBand, GCI_AlphaBand}, 32633, 1600, 1200, 0.25,
             std::nullopt, false, true, false},
            // Without masks, a downsampling of more than 2 sums the taps of each type in GDAL's order
            {"float", GDT_Float32, {GCI_GrayIndex}, 32633, 400, 300, 0.5, std::nullopt, false, false, false},
            {"double", GDT_Float64, {GCI_GrayIndex}, 32633, 400, 300, 0.5, std::nullopt, false, false, false},
            {"int16", GDT_Int16, {GCI_GrayIndex}, 32633, 400, 300, 0.5, std::nullopt, false, false, false},
            {"uint16", GDT_UInt16, {GCI_GrayIndex, GCI_GrayIndex, GCI_GrayIndex, GCI_GrayIndex}, 32633, 400, 300, 0.5,
             std::nullopt, false, false, false},
            // A palette band: its colour table goes to the first band of the output, the entry of the
            // nodata value transparent
            {"palette", GDT_Byte, {GCI_PaletteIndex}, 32633, 400, 300, 0.5, 250.0, false, false, true},
            // Types a normalized copy keeps: Float16, 64-bit integers
            {"half", GDT_Float16, {GCI_GrayIndex}, 32633, 400, 300, 0.5, -9999.0, false, true, false},
            {"int64", GDT_Int64, {GCI_GrayIndex, GCI_GrayIndex}, 32633, 400, 300, 0.5, -5.0, false, false, false},
        };
        for (const Source &s : sources)
        {
            SCOPED_TRACE(s.name);
            const fs::path file = writeSource(ta.getPath(s.name + ".tif"), s);
            std::vector<Render> renders;
            for (const std::string crs : {"EPSG:4326", "EPSG:3857", "EPSG:32633"})
            {
                renders.push_back(render("whole " + crs, crs, extentOf(file, crs, 1.2), 256, 256));
                renders.push_back(render("part " + crs, crs, extentOf(file, crs, 0.3, 0.2, -0.1), 300, 200));
            }
            renders.push_back(render("alpha", "EPSG:3857", extentOf(file, "EPSG:3857", 1.1), 256, 256).withDstAlpha());
            renders.push_back(
                render("white", "EPSG:4326", extentOf(file, "EPSG:4326", 1.1), 256, 256).withDstNodata(255.0));
            renders.push_back(render("near", "EPSG:3857", extentOf(file, "EPSG:3857", 0.5), 200, 200).withNearest());
            renders.push_back(render("bands", "EPSG:3857", extentOf(file, "EPSG:3857", 0.8), 200, 150).withBands({1}));
            renders.push_back(render("small", "EPSG:3857", extentOf(file, "EPSG:3857", 1.0), 64, 48).withDstAlpha());
            renders.push_back(render("tiny", "EPSG:32633", extentOf(file, "EPSG:32633", 1.0), 37, 29));
            // An extent in another CRS than the output's, bounded in it as gdalwarp bounds it
            renders.push_back(render("te_srs 4326", "EPSG:3857", extentOf(file, "EPSG:4326", 0.9, 0.1), 256, 200)
                                  .withExtentCrs("EPSG:4326"));
            renders.push_back(render("te_srs 3857", "EPSG:32633", extentOf(file, "EPSG:3857", 0.7, -0.1, 0.1), 180, 256)
                                  .withExtentCrs("EPSG:3857"));
            renders.push_back(render("te_srs utm", "EPSG:4326", extentOf(file, "EPSG:32633", 1.3), 300, 300)
                                  .withExtentCrs("EPSG:32633"));
            // The warps of the index layer: -ot Float32 -nosrcalpha -setci, the bands of a formula and the
            // alpha band, -dstalpha when the raster says nothing of where it has no pixels
            renders.push_back(render("index", "EPSG:3857", extentOf(file, "EPSG:3857", 1.1), 256, 256).asIndexWarp());
            renders.push_back(
                render("index dstalpha", "EPSG:3857", extentOf(file, "EPSG:3857", 1.1), 256, 256).asIndexWarp().withDstAlpha());
            renders.push_back(render("index small", "EPSG:4326", extentOf(file, "EPSG:4326", 1.0), 60, 45).asIndexWarp());
            for (const Render &r : renders)
                expectWarpedAsGdalWarp(file, r);
        }
    }
    // Real rasters (DDB_WARP_PARITY_FILES, separated by ':'): the whole raster on 1024 x 1024 pixels and
    // 256 x 256 tiles at its centre at 1, 4 and 16 of its pixels per output pixel, in EPSG:3857, with an
    // alpha band
    TEST(TilersWarpParity, rastersFromTheEnvironmentAreWarpedAsGdalWarpWarpsThem)
    {
        const char *files = std::getenv("DDB_WARP_PARITY_FILES");
        if (files == nullptr || *files == 0)
            GTEST_SKIP() << "DDB_WARP_PARITY_FILES is not set";
        std::stringstream list(files);
        for (std::string name; std::getline(list, name, ':');)
        {
            SCOPED_TRACE(name);
            const fs::path file(name);
            const auto whole = extentOf(file, "EPSG:3857", 1.0);
            const auto ds = adoptDataset(GDALOpen(io::toUtf8(file).c_str(), GA_ReadOnly));
            const double pixel = (whole[2] - whole[0]) / GDALGetRasterXSize(datasetHandle(ds));
            const double cx = (whole[0] + whole[2]) / 2, cy = (whole[1] + whole[3]) / 2;
            expectWarpedAsGdalWarp(file, render("whole 1024", "EPSG:3857", whole, 1024, 1024).withDstAlpha());
            for (const int f : {1, 4, 16})
            {
                const double half = 128 * pixel * f;
                expectWarpedAsGdalWarp(file, render("tile x" + std::to_string(f), "EPSG:3857",
                                                    {cx - half, cy - half, cx + half, cy + half}, 256, 256)
                                                 .withDstAlpha());
            }
        }
    }
}
