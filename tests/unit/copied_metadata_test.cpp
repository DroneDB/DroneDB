/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The metadata the rasters libddb writes take from their source, as GDAL's tools copied it when they wrote
// them: the export of bands as gdal_translate, the GeoTIFF of a region render as gdalwarp. Each output is
// compared with the one the tool writes from the same file, with the arguments libddb gave it.

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "raster_test_utils.h"

#include "copied_metadata.h"
#include "mio.h"
#include "ddb.h"
#include "gdal_metadata.h"
#include "gdal_raii.h"
#include "raster_export.h"
#include "raster_region.h"
#include "raster_source.h"
#include "tilers_raster.h"

namespace
{
    using namespace ddb;

    constexpr double kOriginX = 1113000.0, kOriginY = 5621000.0, kPixel = 4.0;
    const char *kXmp = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF/></x:xmpmeta>";

    // A source written by GDAL with what a raster of a user carries: dataset items (TIFF tags among them,
    // and the ones the tools leave out), an XMP packet, and for each band items (statistics among them),
    // a scale and offset, a unit and a description; a colour table on band 1 with `palette`
    struct SourceSpec
    {
        int width = 64, height = 64, bands = 1;
        GDALDataType type = GDT_UInt16;
        bool palette = false;
        bool point = false;  // AREA_OR_POINT=Point: the georeferencing names pixel centres
        std::vector<std::string> interps;
    };

    fs::path writeSource(const fs::path &path, const SourceSpec &spec)
    {
        auto ds = adoptDataset(GDALCreate(GDALGetDriverByName("GTiff"), io::toUtf8(path).c_str(), spec.width,
                                          spec.height, spec.bands, spec.type, nullptr));
        EXPECT_TRUE(ds);
        GDALDatasetH h = datasetHandle(ds);
        if (spec.point)
            GDALSetMetadataItem(h, "AREA_OR_POINT", "Point", nullptr);
        const double gt[6] = {kOriginX, kPixel, 0, kOriginY, 0, -kPixel};
        GDALSetGeoTransform(h, const_cast<double *>(gt));
        OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(srs, 3857);
        GDALSetSpatialRef(h, srs);
        OSRDestroySpatialReference(srs);
        GDALSetMetadataItem(h, "TIFFTAG_SOFTWARE", "a camera", nullptr);
        GDALSetMetadataItem(h, "TIFFTAG_MINSAMPLEVALUE", "3", nullptr);
        GDALSetMetadataItem(h, "CACHE_PATH", "/somewhere", nullptr);
        GDALSetMetadataItem(h, "NITF_BLOCKA_FRLC", "x", nullptr);
        GDALSetMetadataItem(h, "FLIGHT", "a <&> b", nullptr);
        char *xmp[] = {const_cast<char *>(kXmp), nullptr};
        GDALSetMetadata(h, xmp, "xml:XMP");
        std::vector<uint16_t> plane(static_cast<size_t>(spec.width) * spec.height);
        for (int b = 1; b <= spec.bands; b++)
        {
            GDALRasterBandH band = GDALGetRasterBand(h, b);
            if (!spec.interps.empty())
                GDALSetRasterColorInterpretation(band, GDALGetColorInterpretationByName(spec.interps[static_cast<size_t>(b - 1)].c_str()));
            GDALSetRasterScale(band, 0.1 * b);
            GDALSetRasterOffset(band, -5.0 * b);
            GDALSetRasterUnitType(band, b == 2 ? "" : "Celsius");
            GDALSetDescription(band, ("band " + std::to_string(b)).c_str());
            GDALSetMetadataItem(band, "WAVELENGTH", std::to_string(400 + 100 * b).c_str(), nullptr);
            GDALSetMetadataItem(band, "STATISTICS_MINIMUM", "1", nullptr);
            if (b == 1 && spec.palette)
            {
                GDALColorTableH table = GDALCreateColorTable(GPI_RGB);
                // 40 colours of the 256 a Byte table holds, the others black
                for (int i = 0; i < 40; i++)
                {
                    const GDALColorEntry entry{static_cast<short>((i * 61) % 256), static_cast<short>((255 - i * 37) % 256),
                                               static_cast<short>((i * i * 7) % 256), 255};
                    GDALSetColorEntry(table, i, &entry);
                }
                GDALSetRasterColorTable(band, table);
                GDALDestroyColorTable(table);
            }
            for (int y = 0; y < spec.height; y++)
                for (int x = 0; x < spec.width; x++)
                    plane[static_cast<size_t>(y) * spec.width + x] =
                        static_cast<uint16_t>(spec.palette ? (x * 7 + y * 13 + (x * y) % 11) % 40 : 30 + (x * 3 + y * 5 + b * 11) % 200);
            EXPECT_EQ(GDALRasterIO(band, GF_Write, 0, 0, spec.width, spec.height, plane.data(), spec.width, spec.height,
                                   GDT_UInt16, 0, 0),
                      CE_None);
        }
        return path;
    }

    // What GDAL reads of a band besides its pixels
    struct BandInfo
    {
        std::optional<double> scale, offset;
        std::string unit, description, interp;
        std::vector<std::array<short, 4>> colorTable;
        bool operator==(const BandInfo &) const = default;
    };

    BandInfo bandInfo(GDALRasterBandH band)
    {
        BandInfo info;
        int has = 0;
        const double scale = GDALGetRasterScale(band, &has);
        if (has)
            info.scale = scale;
        const double offset = GDALGetRasterOffset(band, &has);
        if (has)
            info.offset = offset;
        info.unit = GDALGetRasterUnitType(band);
        info.description = GDALGetDescription(band);
        info.interp = GDALGetColorInterpretationName(GDALGetRasterColorInterpretation(band));
        if (GDALColorTableH table = GDALGetRasterColorTable(band))
            for (int i = 0; i < GDALGetColorEntryCount(table); i++)
            {
                const GDALColorEntry *e = GDALGetColorEntry(table, i);
                info.colorTable.push_back({e->c1, e->c2, e->c3, e->c4});
            }
        return info;
    }

    std::ostream &operator<<(std::ostream &os, const BandInfo &b)
    {
        return os << "scale " << b.scale.value_or(-1) << " offset " << b.offset.value_or(-1) << " unit '" << b.unit
                  << "' description '" << b.description << "' " << b.interp << " colours " << b.colorTable.size();
    }

    // The metadata of `file` that a tool copies: the default domain and the XMP of the dataset, the items
    // of each band and what GDAL reads of each band
    void expectSameMetadata(const fs::path &want, const fs::path &got)
    {
        const auto w = testutils::openRaster(want);
        const auto g = testutils::openRaster(got);
        ASSERT_TRUE(w && g);
        const gdalmeta::RasterMetadata wm = gdalmeta::metadataOf(datasetHandle(w));
        const gdalmeta::RasterMetadata gm = gdalmeta::metadataOf(datasetHandle(g));
        for (const char *domain : {"", "xml:XMP"})
        {
            const auto items = [domain](const gdalmeta::RasterMetadata &m) {
                for (const auto &d : m.domains)
                    if (d.name == domain)
                        return d.items;
                return std::vector<std::string>();
            };
            EXPECT_EQ(items(gm), items(wm)) << "domain '" << domain << "'";
        }
        EXPECT_EQ(gm.bandItems, wm.bandItems);
        std::array<double, 6> wgt{}, ggt{};
        EXPECT_EQ(GDALGetGeoTransform(datasetHandle(g), ggt.data()), GDALGetGeoTransform(datasetHandle(w), wgt.data()));
        EXPECT_EQ(ggt, wgt) << "the georeferencing GDAL reads";
        ASSERT_EQ(GDALGetRasterCount(datasetHandle(g)), GDALGetRasterCount(datasetHandle(w)));
        for (int b = 1; b <= GDALGetRasterCount(datasetHandle(w)); b++)
            EXPECT_EQ(bandInfo(GDALGetRasterBand(datasetHandle(g), b)), bandInfo(GDALGetRasterBand(datasetHandle(w), b)))
                << "band " << b;
    }

    // The samples of every band and every overview level of `got` are those of `want`
    void expectSamePixels(const fs::path &want, const fs::path &got)
    {
        const auto w = testutils::openRaster(want);
        const auto g = testutils::openRaster(got);
        ASSERT_TRUE(w && g);
        for (int b = 1; b <= GDALGetRasterCount(datasetHandle(w)); b++)
        {
            GDALRasterBandH wb = GDALGetRasterBand(datasetHandle(w), b), gb = GDALGetRasterBand(datasetHandle(g), b);
            ASSERT_EQ(GDALGetOverviewCount(gb), GDALGetOverviewCount(wb)) << "band " << b;
            for (int level = -1; level < GDALGetOverviewCount(wb); level++)
            {
                GDALRasterBandH x = level < 0 ? wb : GDALGetOverview(wb, level);
                GDALRasterBandH y = level < 0 ? gb : GDALGetOverview(gb, level);
                const int lw = GDALGetRasterBandXSize(x), lh = GDALGetRasterBandYSize(x);
                ASSERT_EQ(GDALGetRasterBandXSize(y), lw);
                ASSERT_EQ(GDALGetRasterBandYSize(y), lh);
                std::vector<uint8_t> a(static_cast<size_t>(lw) * lh), z(a.size());
                ASSERT_EQ(GDALRasterIO(x, GF_Read, 0, 0, lw, lh, a.data(), lw, lh, GDT_Byte, 0, 0), CE_None);
                ASSERT_EQ(GDALRasterIO(y, GF_Read, 0, 0, lw, lh, z.data(), lw, lh, GDT_Byte, 0, 0), CE_None);
                size_t differ = 0;
                for (size_t i = 0; i < a.size(); i++)
                    differ += a[i] != z[i];
                EXPECT_EQ(differ, 0u) << "band " << b << " level " << level + 1;
            }
        }
    }

    // The file libddb reads for a raster of a user, which the tools read too
    fs::path readFile(const fs::path &input)
    {
        const auto raster = tilers::Raster::openInput(io::toUtf8(input));
        EXPECT_TRUE(raster);
        return raster ? raster->file() : fs::path();
    }

    class CopiedMetadataTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            DDBRegisterProcess(false);
            area_ = std::make_unique<TestArea>(std::string("CopiedMetadata.") +
                                                   ::testing::UnitTest::GetInstance()->current_test_info()->name(),
                                               true);
        }
        std::unique_ptr<TestArea> area_;
    };

    // ---- The rules, on a source GDAL reads

    TEST(GdalMetadata, anItemIsSplitAsGdalWritersSplitIt)
    {
        using P = std::pair<std::string, std::string>;
        EXPECT_EQ(gdalmeta::parseNameValue("A=1"), P("A", "1"));
        EXPECT_EQ(gdalmeta::parseNameValue("TIME:12=3"), P("TIME", "12=3")) << "the first '=' or ':'";
        EXPECT_EQ(gdalmeta::parseNameValue("A \t= \t b "), P("A", "b ")) << "blanks around the separator only";
        EXPECT_EQ(gdalmeta::parseNameValue("A="), P("A", ""));
        EXPECT_FALSE(gdalmeta::parseNameValue("no separator"));
    }

    TEST_F(CopiedMetadataTest, aTranslationKeepsTheStatisticsAndTheUnitOnlyWhenNeitherTheValuesNorTheExtentChange)
    {
        SourceSpec spec;
        spec.bands = 3;
        const auto ds = testutils::openRaster(writeSource(area_->getPath("source.tif"), spec));
        GDALSetMetadataItem(datasetHandle(ds), "NODATA_VALUES", "0 0 0", nullptr);
        const auto source = rasterSourceOf(datasetHandle(ds));

        const CopiedMetadata same = translatedMetadata(*source, {1, 2, 3}, false, true);
        const auto names = [](const CopiedMetadata::Items &items) {
            std::vector<std::string> out;
            for (const auto &item : items)
                out.push_back(item.first);
            return out;
        };
        EXPECT_EQ(names(same.dataset), (std::vector<std::string>{"TIFFTAG_SOFTWARE", "NITF_BLOCKA_FRLC", "FLIGHT",
                                                                  "NODATA_VALUES", "AREA_OR_POINT"}));
        EXPECT_EQ(same.xmp, kXmp);
        EXPECT_EQ(names(same.bands[0]), (std::vector<std::string>{"WAVELENGTH", "STATISTICS_MINIMUM"}));
        EXPECT_EQ(same.units, (std::vector<std::string>{"Celsius", "", "Celsius"}));
        EXPECT_EQ(same.scales, (std::vector<double>{0.1, 0.2, 0.30000000000000004}));
        EXPECT_EQ(same.descriptions[2], "band 3");

        const CopiedMetadata scaled = translatedMetadata(*source, {1, 2, 3}, true, true);
        EXPECT_EQ(names(scaled.bands[0]), std::vector<std::string>{"WAVELENGTH"});
        EXPECT_EQ(scaled.units, (std::vector<std::string>{"", "", ""}));
        EXPECT_EQ(scaled.offsets, (std::vector<double>{-5, -10, -15})) << "the scale and offset stay";

        const CopiedMetadata window = translatedMetadata(*source, {3, 1}, false, false);
        EXPECT_EQ(names(window.dataset), (std::vector<std::string>{"TIFFTAG_SOFTWARE", "FLIGHT", "AREA_OR_POINT"}))
            << "no NITF block of another extent, no NODATA_VALUES of other bands";
        EXPECT_EQ(window.descriptions, (std::vector<std::string>{"band 3", "band 1"}));
        EXPECT_EQ(window.bands.size(), 2u);
    }

    TEST_F(CopiedMetadataTest, aWarpCopiesTheBandsOnlyWhenItHasAsManyAsItsSource)
    {
        SourceSpec spec;
        spec.bands = 3;
        const auto ds = testutils::openRaster(writeSource(area_->getPath("source.tif"), spec));
        GDALSetMetadataItem(datasetHandle(ds), "NODATA_VALUES", "0 0 0", nullptr);
        const auto source = rasterSourceOf(datasetHandle(ds));

        const CopiedMetadata alpha = warpedMetadata(*source, {}, 4, true);
        EXPECT_EQ(alpha.dataset.size(), 5u) << "no CACHE_PATH, no NODATA_VALUES beside an alpha band";
        EXPECT_TRUE(alpha.bands.empty() && alpha.units.empty() && alpha.descriptions.empty() && alpha.xmp.empty());
        EXPECT_EQ(alpha.scales, (std::vector<double>{0.1, 0.2, 0.30000000000000004, 1.0}));

        const CopiedMetadata picked = warpedMetadata(*source, {3, 1, 2}, 3, false);
        EXPECT_EQ(picked.dataset.size(), 6u);
        EXPECT_EQ(picked.scales, (std::vector<double>{0.30000000000000004, 0.1, 0.2})) << "from the band each one warps";
        EXPECT_EQ(picked.descriptions, (std::vector<std::string>{"band 1", "band 2", "band 3"}))
            << "band by band in their order, as gdalwarp copies them";
    }

    // ---- The outputs, against the tools

    // The export of bands is written as gdal_translate -ot Byte [-srcwin] -b ... [-scale] wrote it from the
    // same file, then given its overviews: its metadata is the one gdal_translate copies
    TEST_F(CopiedMetadataTest, anExportOfBandsHasTheMetadataGdalTranslateCopies)
    {
        struct Case
        {
            std::string name;
            SourceSpec spec;
            std::string bands;
            bool window = false;
        };
        SourceSpec thermal;
        SourceSpec rgb;
        rgb.bands = 3;
        rgb.type = GDT_Byte;
        rgb.interps = {"Red", "Green", "Blue"};
        SourceSpec palette;
        palette.type = GDT_Byte;
        palette.palette = true;
        // Large enough for overviews, which GDAL averages in the colours of the table
        SourceSpec largePalette = palette;
        largePalette.width = 1100;
        largePalette.height = 700;
        SourceSpec point;
        point.point = true;
        const std::vector<Case> cases = {
            {"thermal", thermal, "", false},      {"thermal-window", thermal, "", true}, {"rgb", rgb, "", false},
            {"rgb-window", rgb, "", true},        {"rgb-bands", rgb, "3,1", false},      {"palette", palette, "", false},
            {"point", point, "", false},          {"point-window", point, "", true},
            {"palette-overviews", largePalette, "", false},
        };
        for (const Case &c : cases)
        {
            SCOPED_TRACE(c.name);
            const fs::path input = writeSource(area_->getPath(c.name + ".tif"), c.spec);
            RasterExport request;
            request.input = io::toUtf8(input);
            request.output = io::toUtf8(area_->getPath(c.name + "-ddb.tif"));
            request.bands = c.bands;
            if (c.window)
                // The pixels 8..39 x 16..47 of the source (centres inside the window)
                request.bbox = std::array<double, 4>{9.9985455563, 44.9954740746, 9.9996774336, 44.9962744903};
            ASSERT_NO_THROW(exportRaster(request));

            // gdal_translate with the arguments libddb gave it
            CPLStringList args;
            args.AddString("-ot");
            args.AddString("Byte");
            const auto written = testutils::openRaster(request.output);
            ASSERT_TRUE(written);
            if (c.window)
            {
                // The window libddb exported: the corner of its geotransform, its size
                double gt[6];
                ASSERT_EQ(GDALGetGeoTransform(datasetHandle(written), gt), CE_None);
                args.AddString("-srcwin");
                args.AddString(std::to_string(static_cast<int>(std::lround((gt[0] - kOriginX) / kPixel))).c_str());
                args.AddString(std::to_string(static_cast<int>(std::lround((kOriginY - gt[3]) / kPixel))).c_str());
                args.AddString(std::to_string(GDALGetRasterXSize(datasetHandle(written))).c_str());
                args.AddString(std::to_string(GDALGetRasterYSize(datasetHandle(written))).c_str());
            }
            const std::vector<std::string> bands =
                c.bands.empty() ? std::vector<std::string>(c.spec.bands == 1 ? 1 : 3) : std::vector<std::string>{"3", "1"};
            for (size_t i = 0; i < bands.size(); i++)
            {
                args.AddString("-b");
                args.AddString(c.bands.empty() ? std::to_string(i + 1).c_str() : bands[i].c_str());
            }
            if (c.spec.type != GDT_Byte)
                args.AddString("-scale");
            const fs::path reference = area_->getPath(c.name + "-gdal.tif");
            {
                const GdalOptionsPtr<GDALTranslateOptions> options(GDALTranslateOptionsNew(args.List(), nullptr));
                const auto src = testutils::openRaster(readFile(input));
                ASSERT_TRUE(src);
                auto out = adoptDataset(GDALTranslate(io::toUtf8(reference).c_str(), datasetHandle(src), options.get(), nullptr));
                ASSERT_TRUE(out);
                // and the overviews libddb gave it: power-of-two AVERAGE levels while the larger side is 256 or more
                std::vector<int> levels;
                for (int f = 2; std::max(c.spec.width, c.spec.height) / f >= 256 && levels.size() < 8; f *= 2)
                    levels.push_back(f);
                if (!levels.empty())
                {
                    ASSERT_EQ(GDALBuildOverviews(datasetHandle(out), "AVERAGE", static_cast<int>(levels.size()),
                                                 levels.data(), 0, nullptr, nullptr, nullptr),
                              CE_None);
                }
            }
            expectSameMetadata(reference, request.output);
            expectSamePixels(reference, request.output);
        }
    }

    // An export written without the metadata of its source (the styled picture of a band, the formula) is
    // georeferenced where its source is, a raster of PixelIsPoint included (GDAL's Create writes it
    // PixelIsArea at the corner GDAL reads)
    TEST_F(CopiedMetadataTest, anExportOfAPixelIsPointRasterIsWhereItsSourceIs)
    {
        SourceSpec point;
        point.point = true;
        const fs::path input = writeSource(area_->getPath("point.tif"), point);
        RasterExport request;
        request.input = io::toUtf8(input);
        request.output = io::toUtf8(area_->getPath("styled.tif"));
        request.colormap = "viridis";
        ASSERT_NO_THROW(exportRaster(request));
        const auto src = testutils::openRaster(input);
        const auto out = testutils::openRaster(request.output);
        ASSERT_TRUE(src && out);
        std::array<double, 6> want{}, got{};
        ASSERT_EQ(GDALGetGeoTransform(datasetHandle(src), want.data()), CE_None);
        ASSERT_EQ(GDALGetGeoTransform(datasetHandle(out), got.data()), CE_None);
        EXPECT_EQ(got, want);
        EXPECT_STREQ(GDALGetMetadataItem(datasetHandle(out), "AREA_OR_POINT", nullptr), "Area");
    }

    // The GeoTIFF of a region render has the metadata gdalwarp copied to it from the same file: the items of
    // the dataset and the scale and offset of each band, and the rest of each band when the render has as
    // many bands as the raster (the bands of a request, or a raster with an alpha band)
    TEST_F(CopiedMetadataTest, theGeoTiffOfARegionHasTheMetadataGdalwarpCopies)
    {
        struct Case
        {
            std::string name;
            SourceSpec spec;
            std::vector<int> bands;
        };
        SourceSpec thermal;
        SourceSpec rgba;
        rgba.bands = 4;
        rgba.type = GDT_Byte;
        rgba.interps = {"Red", "Green", "Blue", "Alpha"};
        SourceSpec point;
        point.point = true;
        const std::vector<Case> cases = {{"thermal", thermal, {}},     {"rgba", rgba, {}}, {"rgba-bands", rgba, {2}},
                                         {"rgba-all-bands", rgba, {1, 2, 3, 4}}, {"point", point, {}}};
        const double bbox[4] = {kOriginX + 16, kOriginY - 200, kOriginX + 200, kOriginY - 16};
        for (const Case &c : cases)
        {
            SCOPED_TRACE(c.name);
            const fs::path input = writeSource(area_->getPath(c.name + ".tif"), c.spec);
            uint8_t *buffer = nullptr;
            int size = 0;
            RegionOptions options;
            options.bands = c.bands;
            renderRasterRegion(testutils::regionRequest(io::toUtf8(input), bbox, "EPSG:3857", 32, 32, "image/tiff"),
                               options, &buffer, &size);
            const fs::path rendered = area_->getPath(c.name + "-ddb.tif");
            {
                const std::vector<uint8_t> bytes = testutils::takeBuffer(buffer, size);
                std::ofstream(rendered, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()),
                                                                 static_cast<std::streamsize>(bytes.size()));
            }

            CPLStringList args;
            for (const char *a : {"-of", "GTiff", "-t_srs", "EPSG:3857", "-te_srs", "EPSG:3857", "-ts", "32", "32", "-r",
                                  "bilinear"})
                args.AddString(a);
            args.AddString("-te");
            for (const double v : bbox)
                args.AddString(std::to_string(v).c_str());
            for (const int b : c.bands)
            {
                args.AddString("-b");
                args.AddString(std::to_string(b).c_str());
            }
            if (c.bands.empty())
                args.AddString("-dstalpha");
            const fs::path reference = area_->getPath(c.name + "-gdal.tif");
            {
                const GdalOptionsPtr<GDALWarpAppOptions> warpOptions(GDALWarpAppOptionsNew(args.List(), nullptr));
                const auto src = testutils::openRaster(readFile(input));
                ASSERT_TRUE(src);
                GDALDatasetH h = datasetHandle(src);
                auto out = adoptDataset(GDALWarp(io::toUtf8(reference).c_str(), nullptr, 1, &h, warpOptions.get(), nullptr));
                ASSERT_TRUE(out);
            }
            expectSameMetadata(reference, rendered);
        }
    }

    // An index render asked for as a GeoTIFF is a TIFF of its RGBA pixels (it was a PNG in 2.7.0): four bands,
    // the last one alpha, no georeferencing, the pixels of the "rgba" render
    TEST_F(CopiedMetadataTest, anIndexRenderAskedForAsAGeoTiffIsATiffOfItsPixels)
    {
        SourceSpec ms;
        ms.bands = 5;
        const fs::path input = writeSource(area_->getPath("ms.tif"), ms);
        {
            auto ds = adoptDataset(GDALOpen(io::toUtf8(input).c_str(), GA_Update));
            ASSERT_TRUE(ds);
            const char *names[] = {"Blue", "Green", "Red", "NIR", "RedEdge"};
            for (int b = 1; b <= 5; b++)
                GDALSetDescription(GDALGetRasterBand(datasetHandle(ds), b), names[b - 1]);
        }
        const double bbox[4] = {kOriginX + 16, kOriginY - 200, kOriginX + 200, kOriginY - 16};
        const auto render = [&](const std::string &format) {
            uint8_t *buffer = nullptr;
            int size = 0;
            renderRasterIndex(testutils::regionRequest(io::toUtf8(input), bbox, "EPSG:3857", 24, 20, format), "NDVI",
                              &buffer, &size);
            return testutils::takeBuffer(buffer, size);
        };
        const std::vector<uint8_t> tiff = render("image/tiff");
        const std::vector<uint8_t> rgba = render("rgba");
        ASSERT_GE(tiff.size(), 4u);
        EXPECT_EQ(std::memcmp(tiff.data(), "II*\0", 4), 0) << "a TIFF";
        const fs::path file = area_->getPath("index.tif");
        std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char *>(tiff.data()),
                                                     static_cast<std::streamsize>(tiff.size()));
        const auto ds = testutils::openRaster(file);
        ASSERT_TRUE(ds);
        GDALDatasetH h = datasetHandle(ds);
        ASSERT_EQ(GDALGetRasterCount(h), 4);
        EXPECT_EQ(GDALGetRasterColorInterpretation(GDALGetRasterBand(h, 4)), GCI_AlphaBand);
        double gt[6];
        EXPECT_NE(GDALGetGeoTransform(h, gt), CE_None) << "no georeferencing";
        std::vector<uint8_t> pixels(rgba.size());
        ASSERT_EQ(GDALDatasetRasterIO(h, GF_Read, 0, 0, 24, 20, pixels.data(), 24, 20, GDT_Byte, 4, nullptr, 4, 24 * 4, 1),
                  CE_None);
        EXPECT_EQ(pixels, rgba);
    }

} // namespace
