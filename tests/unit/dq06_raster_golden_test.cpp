/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-06 golden outputs of the raster code: the rendered spectral indices (thumbnail, tile,
// region and export), the formula statistics, and the GeoTIFFs that the mask, COG, merge, align
// and export code write (their structure: compression, predictor, tiling, block size). The
// goldens were taken from the code before DQ-06 and pin it.
//
// Encoded lossy images (WebP, JPEG) are compared by size and transparency; their exact bytes
// are only written to DDB_DQ06_DUMP_DIR, for the exact comparison between two builds.

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "raster_test_utils.h"

#include "align.h"
#include "cog.h"
#include "cog_utils.h"
#include "ddb.h"
#include "dq06_golden_support.h"
#include "dq06_raster_support.h"
#include "gdal_inc.h"
#include "gdaltiler.h"
#include "mask.h"
#include "merge_multispectral.h"
#include "raster_region.h"

namespace
{
    using namespace ddb;

    using dq06::kNodata;
    using dq06::kOriginX;
    using dq06::kOriginY;
    using dq06::kPixel;
    using dq06::kSize;
    using dq06::makeMultispectral;
    using dq06::makeRaster;
    using dq06::reflectance;
    using dq06::setNodata;

    // RGB + alpha, alpha 0 over (30..59, 30..59)
    fs::path makeRgba(const fs::path &path)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = kSize;
        spec.bands = 4;
        spec.type = GDT_Byte;
        spec.alphaBand = true;
        spec.epsg = 3857;
        spec.originX = kOriginX;
        spec.originY = kOriginY;
        spec.pixelSize = kPixel;
        spec.value = [](int band, int x, int y) { return std::floor(reflectance(band, x, y) * 255.0); };
        testutils::createGeoRaster(path, spec);
        std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(path.string().c_str(), GA_Update)));
        std::vector<uint8_t> zero(30 * 30, 0);
        dq06::checkIo(ds->GetRasterBand(4)->RasterIO(GF_Write, 30, 30, 30, 30, zero.data(), 30, 30, GDT_Byte, 0, 0));
        return path;
    }

    // ---- what an output looks like -----------------------------------------------------

    bool isLossy(const json &structure)
    {
        const std::string compression = structure.value("COMPRESSION", "");
        return compression == "JPEG" || compression == "WEBP" || compression == "LERC_ZSTD";
    }

    std::string fileHash(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return dq06::fnv1a(bytes.data(), bytes.size());
    }

    std::string pixelHash(GDALDataset &ds)
    {
        std::vector<double> plane(static_cast<size_t>(ds.GetRasterXSize()) * ds.GetRasterYSize());
        std::string all;
        for (int b = 1; b <= ds.GetRasterCount(); b++)
        {
            dq06::checkIo(ds.GetRasterBand(b)->RasterIO(GF_Read, 0, 0, ds.GetRasterXSize(), ds.GetRasterYSize(), plane.data(),
                                          ds.GetRasterXSize(), ds.GetRasterYSize(), GDT_Float64, 0, 0));
            all += dq06::fnv1a(plane.data(), plane.size() * sizeof(double));
        }
        return dq06::fnv1a(all.data(), all.size());
    }

    // Every band of the raster as float64 values
    std::vector<std::vector<double>> pixelPlanes(GDALDataset &ds)
    {
        const int width = ds.GetRasterXSize(), height = ds.GetRasterYSize();
        std::vector<std::vector<double>> planes;
        for (int b = 1; b <= ds.GetRasterCount(); b++)
        {
            planes.emplace_back(static_cast<size_t>(width) * height);
            dq06::checkIo(ds.GetRasterBand(b)->RasterIO(GF_Read, 0, 0, width, height, planes.back().data(), width, height,
                                                        GDT_Float64, 0, 0));
        }
        return planes;
    }

    // The declared nodata value of every band, if it has one
    std::vector<std::optional<double>> nodataOf(GDALDataset &ds)
    {
        std::vector<std::optional<double>> nodata;
        for (int b = 1; b <= ds.GetRasterCount(); b++)
        {
            int has = 0;
            const double value = ds.GetRasterBand(b)->GetNoDataValue(&has);
            nodata.push_back(has ? std::optional<double>(value) : std::nullopt);
        }
        return nodata;
    }

    bool hasFloatBand(GDALDataset &ds)
    {
        for (int b = 1; b <= ds.GetRasterCount(); b++)
            if (GDALDataTypeIsFloating(ds.GetRasterBand(b)->GetRasterDataType()))
                return true;
        return false;
    }

    // The structure of a GeoTIFF the code wrote: type, bands, tiling and every creation option
    // GDAL reports back. `name` also receives the exact file hash in DDB_DQ06_DUMP_DIR.
    json describeRaster(const std::string &name, const fs::path &path)
    {
        std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(path.string().c_str(), GA_ReadOnly)));
        if (!ds)
            return {{"error", "cannot open"}};
        json out;
        out["driver"] = ds->GetDriverName();
        out["width"] = ds->GetRasterXSize();
        out["height"] = ds->GetRasterYSize();
        json bands = json::array();
        for (int b = 1; b <= ds->GetRasterCount(); b++)
        {
            GDALRasterBand *band = ds->GetRasterBand(b);
            int bx = 0, by = 0;
            band->GetBlockSize(&bx, &by);
            int hasNodata = 0;
            const double nodata = band->GetNoDataValue(&hasNodata);
            json bj = {{"type", GDALGetDataTypeName(band->GetRasterDataType())},
                       {"colorInterp", GDALGetColorInterpretationName(band->GetColorInterpretation())},
                       {"blockX", bx},
                       {"blockY", by},
                       {"overviews", band->GetOverviewCount()},
                       {"maskFlags", band->GetMaskFlags()}};
            if (hasNodata)
                bj["nodata"] = std::isnan(nodata) ? json("nan") : json(nodata);
            bands.push_back(bj);
        }
        out["bands"] = bands;
        json structure = json::object();
        if (CSLConstList md = ds->GetMetadata("IMAGE_STRUCTURE"))
            for (; *md != nullptr; md++)
            {
                const std::string kv = *md;
                const auto eq = kv.find('=');
                structure[kv.substr(0, eq)] = kv.substr(eq + 1);
            }
        out["structure"] = structure;
        out["srs"] = ds->GetSpatialRef() != nullptr ? ds->GetSpatialRef()->GetName() : "";
        std::ifstream in(path, std::ios::binary);
        char magic[4] = {0, 0, 0, 0};
        in.read(magic, 4);
        out["bigTiff"] = (magic[2] == 0x2B);

        const std::string hash = pixelHash(*ds);
        if (!isLossy(structure))
        {
            // Integer pixels are exact on every platform; the floats of a warp can differ in their
            // last bit, so a float raster has a golden file of its pixels, compared with a tolerance
            if (hasFloatBand(*ds))
            {
                out["pixelsFile"] = name;
                dq06::expectPixelsNear(name, ds->GetRasterXSize(), ds->GetRasterYSize(), pixelPlanes(*ds), nodataOf(*ds));
            }
            else
                out["pixels"] = hash;
        }
        dq06::dumpOnly(name + "_exact", {{"file", fileHash(path)}, {"pixels", hash}});
        return out;
    }

    // A decoded image: its size, transparency and, when lossless, its pixels
    json describeImage(const std::string &name, const std::vector<uint8_t> &bytes, bool lossless)
    {
        const testutils::DecodedImage img = testutils::decodeImage(bytes);
        json out = {{"width", img.width}, {"height", img.height}, {"bands", img.bands.size()},
                    {"alpha", img.hasAlpha()}};
        if (img.hasAlpha())
        {
            size_t transparent = 0;
            for (const uint8_t a : img.alpha())
                transparent += (a == 0);
            out["transparent"] = transparent;
        }
        std::string all;
        for (const auto &plane : img.bands)
            all += dq06::fnv1a(plane.data(), plane.size());
        const std::string hash = dq06::fnv1a(all.data(), all.size());
        if (lossless)
            out["pixels"] = hash;
        dq06::dumpOnly(name + "_exact", {{"bytes", dq06::fnv1a(bytes.data(), bytes.size())}, {"pixels", hash}});
        return out;
    }

    // `message` with the folder of the test replaced by "<dir>"
    std::string scrubbed(const std::string &message, TestArea &ta)
    {
        return dq06::scrubFolder(message, ta.getFolder().string());
    }

    bool isPng(const std::vector<uint8_t> &b) { return b.size() > 4 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G'; }

    std::vector<uint8_t> take(uint8_t *buffer, int size) { return testutils::takeBuffer(buffer, size); }

    class Dq06Raster : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    // ---- spectral indices --------------------------------------------------------------

    // The region of the raster plus a margin that falls outside it
    const double kBbox[4] = {kOriginX - 64.0, kOriginY - kSize * kPixel - 64.0, kOriginX + kSize * kPixel + 64.0,
                             kOriginY + 64.0};

    TEST_F(Dq06Raster, renderRasterIndex)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path ms = makeMultispectral(ta.getPath("ms.tif"));
        json out = json::object();
        for (const char *index : {"NDVI", "NDRE", "NDWI", "SAVI", "EVI", "ndvi"})
        {
            for (const char *format : {"image/png", "image/tiff", "image/jpeg"})
            {
                uint8_t *buffer = nullptr;
                int size = 0;
                const std::string key = std::string(index) + "_" + format;
                try
                {
                    renderRasterIndex(ms.string(), index, kBbox, "EPSG:3857", 80, 80, format, &buffer, &size);
                }
                catch (const std::exception &e)
                {
                    out[key] = {{"error", e.what()}};
                    continue;
                }
                out[key] = describeImage("render_index_" + key, take(buffer, size), std::string(format) != "image/jpeg");
            }
        }
        // Not a raster index, and a raster without the bands
        uint8_t *buffer = nullptr;
        int size = 0;
        try
        {
            renderRasterIndex(ms.string(), "NOPE", kBbox, "EPSG:3857", 16, 16, "image/png", &buffer, &size);
        }
        catch (const std::exception &e)
        {
            out["unknown"] = {{"error", e.what()}};
        }
        const fs::path rgba = makeRgba(ta.getPath("rgba.tif"));
        try
        {
            renderRasterIndex(rgba.string(), "NDRE", kBbox, "EPSG:3857", 16, 16, "image/png", &buffer, &size);
        }
        catch (const std::exception &e)
        {
            out["fewBands"] = {{"error", e.what()}};
        }
        dq06::expectGolden("render_raster_index", out);
    }

    struct Formula
    {
        const char *formula;
        const char *filter;
        const char *colormap;
        const char *rescale;
    };

    const std::vector<Formula> kFormulas = {
        {"NDVI", "RGBReN", nullptr, nullptr},      {"SAVI", "RGBReN", "viridis", nullptr},
        {"EVI", "RGBReN", "spectral", "-0.5,1"},   {"NDRE", "RGBReN", "rdylgn", nullptr},
        {"GLI", "RGB", "rdylgn", nullptr},         {"VARI", "RGB", "plasma", nullptr},
        {"EXG", "RGB", "grayscale", nullptr},      {"GRVI", "RGBReN", "inferno", "0,3"},
        {"NDWI", "RGBReN", "bugn", nullptr},       {"NOPE", "RGB", nullptr, nullptr},
    };

    TEST_F(Dq06Raster, thumbnailAndTileFormulas)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path ms = makeMultispectral(ta.getPath("ms.tif"));
        json out = json::object();
        for (const auto &f : kFormulas)
        {
            const std::string key = std::string(f.formula) + "_" + (f.colormap ? f.colormap : "default") + "_" +
                                    (f.rescale ? "rescale" : "auto");
            uint8_t *buffer = nullptr;
            int size = 0;
            const DDBErr err = DDBGenerateMemoryThumbnailEx(ms.string().c_str(), 96, nullptr, nullptr, f.formula,
                                                            f.filter, f.colormap, f.rescale, &buffer, &size);
            if (err != DDBERR_NONE)
            {
                out["thumb_" + key] = {{"error", DDBGetLastError()}};
                continue;
            }
            const auto bytes = take(buffer, size);
            out["thumb_" + key] = describeImage("thumb_" + key, bytes, isPng(bytes));
        }

        // Every tile of the raster at its deepest zoom level
        GDALTiler tiler(ms.string(), "", 256, false);
        const auto zooms = tiler.getMinMaxZ();
        const auto bounds = tiler.getMinMaxCoordsForZ(zooms.max);
        for (const auto &f : kFormulas)
        {
            if (std::string(f.formula) == "NOPE")
                continue;
            for (int tx = bounds.min.x; tx <= bounds.max.x; tx++)
            {
                for (int ty = bounds.min.y; ty <= bounds.max.y; ty++)
                {
                    const std::string key = std::string(f.formula) + "_" + (f.colormap ? f.colormap : "default") + "_" +
                                            std::to_string(tx) + "_" + std::to_string(ty);
                    uint8_t *buffer = nullptr;
                    int size = 0;
                    const DDBErr err = DDBMemoryTileEx(ms.string().c_str(), zooms.max, tx, ty, 256, false, true, "dq06",
                                                       nullptr, nullptr, f.formula, f.filter, f.colormap, f.rescale,
                                                       &buffer, &size);
                    if (err != DDBERR_NONE)
                    {
                        out["tile_" + key] = {{"error", DDBGetLastError()}};
                        continue;
                    }
                    const auto bytes = take(buffer, size);
                    out["tile_" + key] = describeImage("tile_" + key, bytes, isPng(bytes));
                }
            }
        }
        dq06::expectGolden("thumbnail_and_tile_formulas", out);
    }

    TEST_F(Dq06Raster, formulaExport)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path ms = makeMultispectral(ta.getPath("ms.tif"));
        json out = json::object();
        struct Case
        {
            const char *name;
            const char *formula;
            const char *filter;
            const char *colormap;
            const char *rescale;
            int tile;
        };
        const std::vector<Case> cases = {
            {"ndvi_range", "NDVI", "RGBReN", nullptr, nullptr, 0},   // the formula's own range
            {"ndvi_rescale", "NDVI", "RGBReN", "viridis", "-0.2,0.8", 64},
            {"exg_percentile", "EXG", "RGB", "magma", nullptr, 0},    // no range: p2-p98 of the values
            {"grvi_tiled", "GRVI", "RGBReN", nullptr, nullptr, 64},   // percentile over several tiles
            {"unknown", "NOPE", "RGB", nullptr, nullptr, 0},
            {"badrescale", "NDVI", "RGBReN", nullptr, "oops", 0},
        };
        for (const auto &c : cases)
        {
            const fs::path output = ta.getPath(std::string("export_") + c.name + ".tif");
            const DDBErr err = DDBExportRaster2(ms.string().c_str(), output.string().c_str(), nullptr, nullptr,
                                                c.formula, c.filter, c.colormap, c.rescale, c.tile, nullptr, nullptr);
            out[c.name] = err == DDBERR_NONE ? describeRaster(std::string("export_") + c.name, output)
                                             : json{{"error", DDBGetLastError()}};
        }

        // Band selection and rescale, no formula
        const fs::path banded = ta.getPath("export_bands.tif");
        const DDBErr err = DDBExportRaster2(ms.string().c_str(), banded.string().c_str(), nullptr, "5,4,1", nullptr,
                                            nullptr, nullptr, "0,0.6", 0, nullptr, nullptr);
        out["bands"] = err == DDBERR_NONE ? describeRaster("export_bands", banded) : json{{"error", DDBGetLastError()}};
        dq06::expectGolden("formula_export", out);
    }

    TEST_F(Dq06Raster, formulaStatistics)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path ms = makeMultispectral(ta.getPath("ms.tif"));
        const fs::path rgba = makeRgba(ta.getPath("rgba.tif"));
        json out = json::object();
        struct Case
        {
            const char *name;
            const fs::path *path;
            const char *formula;
            const char *filter;
        };
        const std::vector<Case> cases = {{"ndvi", &ms, "NDVI", "RGBReN"},
                                         {"savi", &ms, "SAVI", "RGBReN"},
                                         {"plain", &ms, nullptr, nullptr},
                                         {"vari_alpha", &rgba, "VARI", "RGB"},
                                         {"gli_alpha", &rgba, "GLI", "RGB"}};
        for (const auto &c : cases)
        {
            char *buffer = nullptr;
            const DDBErr err = DDBGetRasterMetadata(c.path->string().c_str(), c.formula, c.filter, &buffer);
            if (err != DDBERR_NONE)
            {
                out[c.name] = {{"error", DDBGetLastError()}};
                continue;
            }
            json j = json::parse(buffer);
            DDBFree(buffer);
            out[c.name] = j;
        }
        dq06::expectGolden("formula_statistics", out);
    }

    // The RGB + alpha raster through the formula render: alpha 0 is transparent
    TEST_F(Dq06Raster, alphaIsTransparentInFormulas)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path rgba = makeRgba(ta.getPath("rgba.tif"));
        uint8_t *buffer = nullptr;
        int size = 0;
        ASSERT_EQ(DDBGenerateMemoryThumbnailEx(rgba.string().c_str(), 64, nullptr, nullptr, "VARI", "RGB", nullptr,
                                               nullptr, &buffer, &size),
                  DDBERR_NONE);
        const auto bytes = take(buffer, size);
        dq06::expectGolden("alpha_formula_thumbnail", describeImage("alpha_formula_thumbnail", bytes, isPng(bytes)));
    }

    // ---- GeoTIFF creation options ------------------------------------------------------

    // A raster without georeference
    fs::path makePlain(const fs::path &path, GDALDataType type, int size)
    {
        GDALDriver *drv = GetGDALDriverManager()->GetDriverByName("GTiff");
        std::unique_ptr<GDALDataset> ds(drv->Create(path.string().c_str(), size, size, 1, type, nullptr));
        std::vector<double> row(size);
        for (int y = 0; y < size; y++)
        {
            for (int x = 0; x < size; x++)
                row[x] = (x * 5 + y * 3) % 200;
            dq06::checkIo(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, y, size, 1, row.data(), size, 1, GDT_Float64, 0, 0));
        }
        return path;
    }

    struct SourceCase
    {
        const char *name;
        int bands;
        GDALDataType type;
        bool alpha;
        std::optional<double> nodata;
    };

    const std::vector<SourceCase> kSources = {
        {"rgb8", 3, GDT_Byte, false, std::nullopt},   {"gray8", 1, GDT_Byte, false, std::nullopt},
        {"rgba8", 4, GDT_Byte, true, std::nullopt},   {"rgbn8", 4, GDT_Byte, false, std::nullopt},
        {"rgb8_nodata", 3, GDT_Byte, false, 0.0},     {"dem32", 1, GDT_Float32, false, kNodata},
        {"dem64", 1, GDT_Float64, false, std::nullopt}, {"u16", 1, GDT_UInt16, false, std::nullopt},
        {"rgb16", 3, GDT_UInt16, false, std::nullopt},
    };

    TEST_F(Dq06Raster, maskBordersOutputs)
    {
        TestArea ta(TEST_NAME, true);
        json out = json::object();
        for (const auto &s : kSources)
        {
            const fs::path input = makeRaster(ta.getPath(std::string(s.name) + ".tif"), s.bands, s.type, s.alpha, s.nodata);
            const fs::path output = ta.getPath(std::string(s.name) + "_masked.tif");
            try
            {
                maskBorders(input.string(), output.string(), 15, false, "");
                out[s.name] = describeRaster(std::string("mask_") + s.name, output);
            }
            catch (const std::exception &e)
            {
                out[s.name] = {{"error", scrubbed(e.what(), ta)}};
            }
        }
        dq06::expectGolden("mask_borders_outputs", out);
    }

    TEST_F(Dq06Raster, buildCogOutputs)
    {
        TestArea ta(TEST_NAME, true);
        json out = json::object();
        for (const auto &s : kSources)
        {
            const fs::path input = makeRaster(ta.getPath(std::string(s.name) + ".tif"), s.bands, s.type, s.alpha, s.nodata);
            const fs::path output = ta.getPath(std::string(s.name) + "_cog.tif");
            try
            {
                buildCog(input.string(), output.string());
                json j = describeRaster(std::string("cog_") + s.name, output);
                j["optimized"] = isOptimizedCog(output.string());
                out[s.name] = j;
            }
            catch (const std::exception &e)
            {
                out[s.name] = {{"error", scrubbed(e.what(), ta)}};
            }
        }
        dq06::expectGolden("build_cog_outputs", out);
    }

    // Single-band rasters of one scene, merged into one multi-band GeoTIFF
    TEST_F(Dq06Raster, mergeMultispectralOutputs)
    {
        TestArea ta(TEST_NAME, true);
        json out = json::object();
        struct Merge
        {
            const char *name;
            GDALDataType type;
            int inputs;
            bool georeferenced;
        };
        const std::vector<Merge> merges = {{"byte3", GDT_Byte, 3, true},   {"byte4", GDT_Byte, 4, true},
                                           {"byte5", GDT_Byte, 5, true},   {"float5", GDT_Float32, 5, true},
                                           {"uint16_3", GDT_UInt16, 3, true}, {"byte3_plain", GDT_Byte, 3, false},
                                           {"float4_plain", GDT_Float32, 4, false}};
        for (const auto &m : merges)
        {
            std::vector<std::string> inputs;
            for (int i = 0; i < m.inputs; i++)
            {
                const fs::path p = ta.getPath(std::string(m.name) + "_in" + std::to_string(i) + ".tif");
                if (m.georeferenced)
                    makeRaster(p, 1, m.type, false, std::nullopt, 64);
                else
                    makePlain(p, m.type, 64);
                inputs.push_back(p.string());
            }
            const fs::path output = ta.getPath(std::string(m.name) + "_merged.tif");
            try
            {
                mergeMultispectral(inputs, output.string());
                out[m.name] = describeRaster(std::string("merge_") + m.name, output);
            }
            catch (const std::exception &e)
            {
                out[m.name] = {{"error", scrubbed(e.what(), ta)}};
            }
        }
        dq06::expectGolden("merge_multispectral_outputs", out);
    }

    // The aligned output of a DEM and of an orthophoto (a COG after the temporary GeoTIFF)
    TEST_F(Dq06Raster, alignOutputs)
    {
        TestArea ta(TEST_NAME, true);
        json out = json::object();
        for (const bool dem : {true, false})
        {
            const std::string name = dem ? "dem" : "ortho";
            const fs::path ref = makeRaster(ta.getPath(name + "_ref.tif"), dem ? 1 : 3, dem ? GDT_Float32 : GDT_Byte,
                                            false, dem ? std::optional<double>(kNodata) : std::nullopt, 256);
            const fs::path src = ta.getPath(name + "_src.tif");
            makeRaster(src, dem ? 1 : 3, dem ? GDT_Float32 : GDT_Byte, false,
                       dem ? std::optional<double>(kNodata) : std::nullopt, 256);
            {
                std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset *>(GDALOpen(src.string().c_str(), GA_Update)));
                const double gt[6] = {500003.0, 1.0, 0.0, 4499998.0, 0.0, -1.0};
                ds->SetGeoTransform(const_cast<double *>(gt));
                if (dem)
                {
                    // 5 m higher than the reference, with a nodata patch
                    std::vector<float> plane(256 * 256);
                    GDALRasterBand *band = ds->GetRasterBand(1);
                    dq06::checkIo(band->RasterIO(GF_Read, 0, 0, 256, 256, plane.data(), 256, 256, GDT_Float32, 0, 0));
                    for (int y = 0; y < 256; y++)
                        for (int x = 0; x < 256; x++)
                            plane[y * 256 + x] = (x < 10 && y < 10) ? static_cast<float>(kNodata) : plane[y * 256 + x] + 5.0f;
                    dq06::checkIo(band->RasterIO(GF_Write, 0, 0, 256, 256, plane.data(), 256, 256, GDT_Float32, 0, 0));
                }
            }
            const fs::path output = ta.getPath(name + "_aligned.tif");
            AlignOptions options;
            options.mode = AlignMode::Translation;
            try
            {
                const AlignResult r = alignRaster(src.string(), ref.string(), output.string(), options);
                json j = describeRaster(std::string("align_") + name, output);
                j["mode"] = r.mode;
                j["shiftZ"] = r.shiftZ;
                j["success"] = r.success;
                out[name] = j;
            }
            catch (const std::exception &e)
            {
                out[name] = {{"error", scrubbed(e.what(), ta)}};
            }
        }
        dq06::expectGolden("align_outputs", out);
    }
}
