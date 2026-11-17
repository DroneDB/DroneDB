/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The COG of a GeoRaster built before the rules of CogBuildPolicy is built again only when the COG
// built now differs from it: for a source without nodata, mask or alpha band, with the nodata of
// several bands, or with a colour table, and for a COG that lies elsewhere than its raster (a CRS
// the builds before read wrongly, a raster around a pole cut at its edges). The probe records the
// nodata of the bands and the mask of the raster, so the index decides; for an entry indexed
// before, the header of the COG it has does; the header of a COG the facts find healthy says where
// it lies. Every API that reports the state of a build agrees (DDBIsBuildComplete, the manifest the
// completeness sweep of Registry reads, the listing with builds, the status).

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#ifndef WIN32
#include <unistd.h>
#endif

#include "build_layout.h"
#include "buildlock.h"
#include "cog_build_policy.h"
#include "contract_fixtures.h"
#include "cpl_conv.h"
#include "ddb.h"
#include "dbops.h"
#include "entry_handler.h"
#include "gdal_raii.h"
#include "gdal_utils.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {
using namespace ddb;

constexpr int kWidth = 64, kHeight = 48;

struct Source {
    const char* name;
    int bands;
    GDALDataType type;
    bool alpha;                    // the last band is an alpha band
    std::optional<double> noData;  // of every band but the alpha band
    bool mask;                     // an internal mask
    bool palette;                  // band 1 has a colour table
    bool outdated;                 // a COG built before the rules differs from the one built now
    int epsg = 32632;              // where it lies: its CRS, the corner of its first pixel and the size of a pixel
    double originX = 500000.0, originY = 5000000.0, pixel = 2.0;
};

const std::vector<Source> kSources = {
    {"rgb", 3, GDT_Byte, false, std::nullopt, false, false, true},
    {"dem-without-nodata", 1, GDT_Float32, false, std::nullopt, false, false, true},
    {"rgb-nodata", 3, GDT_Byte, false, 0.0, false, false, true},
    {"palette", 1, GDT_Byte, false, std::nullopt, false, true, true},
    {"rgba", 4, GDT_Byte, true, std::nullopt, false, false, false},
    {"rgba-nodata", 4, GDT_Byte, true, 0.0, false, false, false},
    {"dem-nodata", 1, GDT_Float32, false, -9999.0, false, false, false},
    {"dem-nan-nodata", 1, GDT_Float32, false, std::numeric_limits<double>::quiet_NaN(), false, false, false},
    {"rgb-mask", 3, GDT_Byte, false, std::nullopt, true, false, false},
};

// A GeoTIFF (UTM 32N unless `s` says otherwise; its COG is warped to the tiles of EPSG:3857) with
// the transparency of `s`
fs::path writeSource(const fs::path& path, const Source& s) {
    CPLSetThreadLocalConfigOption("GDAL_TIFF_INTERNAL_MASK", "YES");
    {
        auto ds = adoptDataset(GDALCreate(GDALGetDriverByName("GTiff"), io::toUtf8(path).c_str(), kWidth, kHeight,
                                          s.bands, s.type, nullptr));
        EXPECT_TRUE(ds);
        GDALDatasetH h = datasetHandle(ds);
        double gt[6] = {s.originX, s.pixel, 0.0, s.originY, 0.0, -s.pixel};
        GDALSetGeoTransform(h, gt);
        SrsPtr srs(OSRNewSpatialReference(nullptr));
        OSRImportFromEPSG(srs.get(), s.epsg);
        GDALSetSpatialRef(h, srs.get());
        // Before the pixels: GTiff writes the photometric interpretation with the first block
        if (s.palette) {
            GDALColorTableH table = GDALCreateColorTable(GPI_RGB);
            for (int i = 0; i < 64; i++) {
                const GDALColorEntry e{static_cast<short>(i * 4), static_cast<short>(255 - i * 4), 90, 255};
                GDALSetColorEntry(table, i, &e);
            }
            GDALSetRasterColorTable(GDALGetRasterBand(h, 1), table);
            GDALDestroyColorTable(table);
        }
        std::vector<float> plane(static_cast<size_t>(kWidth) * kHeight);
        for (int b = 1; b <= s.bands; b++) {
            GDALRasterBandH band = GDALGetRasterBand(h, b);
            const bool alpha = s.alpha && b == s.bands;
            for (int y = 0; y < kHeight; y++)
                for (int x = 0; x < kWidth; x++)
                    plane[static_cast<size_t>(y) * kWidth + x] =
                        alpha ? (x < 8 ? 0.0f : 255.0f) : static_cast<float>(10 + (x * 3 + y * 5 + b * 40) % 30);
            EXPECT_EQ(GDALRasterIO(band, GF_Write, 0, 0, kWidth, kHeight, plane.data(), kWidth, kHeight, GDT_Float32,
                                   0, 0),
                      CE_None);
            if (alpha)
                GDALSetRasterColorInterpretation(band, GCI_AlphaBand);
            else if (s.noData)
                GDALSetRasterNoDataValue(band, *s.noData);
        }
        if (s.mask) {
            EXPECT_EQ(GDALCreateDatasetMaskBand(h, GMF_PER_DATASET), CE_None);
            std::vector<uint8_t> mask(static_cast<size_t>(kWidth) * kHeight, 255);
            for (int y = 0; y < kHeight; y++)
                for (int x = 0; x < 8; x++)
                    mask[static_cast<size_t>(y) * kWidth + x] = 0;
            EXPECT_EQ(GDALRasterIO(GDALGetMaskBand(GDALGetRasterBand(h, 1)), GF_Write, 0, 0, kWidth, kHeight,
                                   mask.data(), kWidth, kHeight, GDT_Byte, 0, 0),
                      CE_None);
        }
    }
    CPLSetThreadLocalConfigOption("GDAL_TIFF_INTERNAL_MASK", nullptr);
    return path;
}

// The COG a build before the rules made of `source`: GDAL's COG of it, which keeps its nodata, mask,
// alpha band and colour table as those builds kept them
void writeOldCog(const fs::path& source, const fs::path& cog) {
    auto src = adoptDataset(GDALOpen(io::toUtf8(source).c_str(), GA_ReadOnly));
    ASSERT_TRUE(src);
    io::assureIsRemoved(cog);
    auto out = adoptDataset(GDALCreateCopy(GDALGetDriverByName("COG"), io::toUtf8(cog).c_str(), datasetHandle(src),
                                           FALSE, nullptr, nullptr, nullptr));
    ASSERT_TRUE(out);
}

// The COG a build before the rules made of `source` on the tiles of EPSG:3857, as those builds warped
// it (gdalwarp -of COG -t_srs EPSG:3857 -co TILING_SCHEME=GoogleMapsCompatible); `sourceSrs`, when
// set, is the CRS they read for it instead of its own, and `shiftX` moves the COG that many metres
// east, as a CRS read wrongly moved it
void writeOldGridCog(const fs::path& source, const fs::path& cog, const std::string& sourceSrs = "",
                     double shiftX = 0) {
    auto src = adoptDataset(GDALOpen(io::toUtf8(source).c_str(), GA_ReadOnly));
    ASSERT_TRUE(src);
    io::assureIsRemoved(cog);
    const fs::path warped = shiftX != 0 ? fs::path(io::toUtf8(cog) + ".grid.tif") : cog;
    std::vector<std::string> args = {"-of",        "COG", "-t_srs", "EPSG:3857", "-r", "bilinear",
                                     "-co",        "TILING_SCHEME=GoogleMapsCompatible",
                                     "-co",        "ZOOM_LEVEL_STRATEGY=UPPER"};
    if (!sourceSrs.empty())
        args.insert(args.end(), {"-s_srs", sourceSrs});
    {
        CPLStringList list = cslFromStrings(args);
        const GdalOptionsPtr<GDALWarpAppOptions> options(GDALWarpAppOptionsNew(list.List(), nullptr));
        ASSERT_TRUE(options);
        GDALDatasetH h = datasetHandle(src);
        auto out = adoptDataset(GDALWarp(io::toUtf8(warped).c_str(), nullptr, 1, &h, options.get(), nullptr));
        ASSERT_TRUE(out);
    }
    if (shiftX == 0)
        return;
    {
        auto grid = adoptDataset(GDALOpen(io::toUtf8(warped).c_str(), GA_ReadOnly));
        ASSERT_TRUE(grid);
        double gt[6];
        ASSERT_EQ(GDALGetGeoTransform(datasetHandle(grid), gt), CE_None);
        const double w = GDALGetRasterXSize(datasetHandle(grid)), h = GDALGetRasterYSize(datasetHandle(grid));
        const std::vector<std::string> shift = {"-of",
                                                "COG",
                                                "-a_ullr",
                                                std::to_string(gt[0] + shiftX),
                                                std::to_string(gt[3]),
                                                std::to_string(gt[0] + w * gt[1] + shiftX),
                                                std::to_string(gt[3] + h * gt[5])};
        CPLStringList list = cslFromStrings(shift);
        const GdalOptionsPtr<GDALTranslateOptions> options(GDALTranslateOptionsNew(list.List(), nullptr));
        ASSERT_TRUE(options);
        auto out = adoptDataset(GDALTranslate(io::toUtf8(cog).c_str(), datasetHandle(grid), options.get(), nullptr));
        ASSERT_TRUE(out);
    }
    fs::remove(warped);
}

// The WKT of an EPSG code
std::string wktOfEpsg(int epsg) {
    const SrsPtr srs = srsFromEpsg(epsg);
    char* wkt = nullptr;
    OSRExportToWkt(srs.get(), &wkt);
    std::string text = wkt != nullptr ? wkt : "";
    CPLFree(wkt);
    return text;
}

// Web Mercator (EPSG:3857) of a longitude and a latitude in degrees
std::array<double, 2> mercator(double lon, double lat) {
    constexpr double r = 6378137.0, rad = 3.14159265358979323846 / 180.0;
    return {r * lon * rad, r * std::log(std::tan(3.14159265358979323846 / 4 + lat * rad / 2))};
}

// The ring of a box of longitudes and latitudes, as the probe writes a footprint
BasicPolygonGeometry footprintOf(double west, double south, double east, double north) {
    BasicPolygonGeometry g;
    for (const auto& [x, y] : {std::pair{west, north}, {east, north}, {east, south}, {west, south}, {west, north}})
        g.addPoint(x, y, 0.0);
    return g;
}

// A COG of EPSG:3857 whose grid covers a box of longitudes and latitudes, of 1 m pixels
BuiltCog cogOver(double west, double south, double east, double north) {
    BuiltCog cog;
    cog.wkt = wktOfEpsg(3857);
    const auto sw = mercator(west, south), ne = mercator(east, north);
    cog.extent = {sw[0], sw[1], ne[0], ne[1]};
    cog.pixelSize = 1.0;
    return cog;
}

// Whether every API that reports the state of the build of `path` says it is complete; the test
// fails when they do not agree
bool reportedComplete(const std::string& root, const std::string& path) {
    bool complete = false;
    EXPECT_EQ(DDBIsBuildComplete(root.c_str(), path.c_str(), &complete), DDBERR_NONE) << DDBGetLastError();

    const char* paths[] = {path.c_str()};
    char* out = nullptr;
    EXPECT_EQ(DDBGetBuildArtifacts(root.c_str(), paths, 1, &out), DDBERR_NONE) << DDBGetLastError();
    const json artifacts = contract::takeJson(out);
    EXPECT_EQ(artifacts[0]["build"]["state"], complete ? "complete" : "none") << path << ": " << artifacts.dump();
    // The COG it has is served meanwhile
    EXPECT_EQ(artifacts[0]["build"]["artifacts"].size(), 1u) << path << ": " << artifacts.dump();

    DDBListEntriesOptions options{sizeof(DDBListEntriesOptions), true, false, true, 0};
    EXPECT_EQ(DDBListEntriesEx(root.c_str(), path.c_str(), "", "", &options, &out), DDBERR_NONE) << DDBGetLastError();
    const json listed = contract::takeJson(out);
    EXPECT_EQ(listed.size(), 1u) << listed.dump();
    if (listed.size() == 1) {
        EXPECT_EQ(listed[0]["build"]["state"], complete ? "complete" : "none") << path << ": " << listed.dump();
    }

    EXPECT_EQ(DDBGetBuildStatus(root.c_str(), paths, 1, &out), DDBERR_NONE) << DDBGetLastError();
    const json status = contract::takeJson(out);
    EXPECT_EQ(status[0]["complete"], complete) << path << ": " << status.dump();
    return complete;
}

}  // namespace

TEST(CogBuildPolicy, outdatesTheCogOfASourceWithoutTransparencyWithTheNodataOfSeveralBandsOrWithAPalette) {
    const auto outdates = [](bool palette, bool alpha, bool mask, int noDataBands) {
        return CogBuildPolicy::outdates(RasterTransparency{palette, alpha, mask, noDataBands});
    };
    EXPECT_TRUE(outdates(false, false, false, 0)) << "no transparency";
    EXPECT_TRUE(outdates(false, false, false, 3)) << "the nodata of several bands";
    EXPECT_TRUE(outdates(false, false, true, 3)) << "the nodata of several bands beside a mask";
    EXPECT_TRUE(outdates(true, false, false, 0)) << "a palette";
    EXPECT_TRUE(outdates(true, true, false, 0)) << "a palette beside an alpha band";
    EXPECT_FALSE(outdates(false, true, false, 0)) << "an alpha band";
    EXPECT_FALSE(outdates(false, true, false, 3)) << "an alpha band beside the nodata of the other bands";
    EXPECT_FALSE(outdates(false, false, false, 1)) << "the nodata of one band";
    EXPECT_FALSE(outdates(false, false, true, 0)) << "a mask";
    EXPECT_FALSE(outdates(false, false, true, 1)) << "a mask beside the nodata of one band";
}

TEST(CogBuildPolicy, theIndexDecidesWhenItHoldsTheFactsOrTheBandsAreAPaletteOrHaveAnAlpha) {
    const auto band = [](const char* colorInterp) { return json{{"type", "Byte"}, {"colorInterp", colorInterp}}; };
    json withNoData = band("Gray");
    withNoData["noData"] = "nan";

    // Indexed by this release: every fact
    auto t = CogBuildPolicy::indexed({{"bands", {withNoData}}, {"hasMask", false}});
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->noDataBands, 1);
    EXPECT_FALSE(t->mask);
    t = CogBuildPolicy::indexed({{"bands", {band("Red"), band("Green"), band("Blue")}}, {"hasMask", true}});
    ASSERT_TRUE(t.has_value());
    EXPECT_TRUE(t->mask);
    EXPECT_EQ(t->noDataBands, 0);

    // Indexed before: the colour interpretations decide for a palette or an alpha band
    t = CogBuildPolicy::indexed({{"bands", {band("Palette")}}});
    ASSERT_TRUE(t.has_value());
    EXPECT_TRUE(t->palette);
    t = CogBuildPolicy::indexed({{"bands", {band("Red"), band("Green"), band("Blue"), band("Alpha")}}});
    ASSERT_TRUE(t.has_value());
    EXPECT_TRUE(t->alpha);
    EXPECT_FALSE(CogBuildPolicy::indexed({{"bands", {band("Red"), band("Green"), band("Blue")}}}).has_value());
    EXPECT_FALSE(CogBuildPolicy::indexed({{"bands", {band("Gray")}}}).has_value());
    EXPECT_FALSE(CogBuildPolicy::indexed(json::object()).has_value());
    EXPECT_FALSE(CogBuildPolicy::indexed(json()).has_value());
}

TEST(CogBuildPolicy, theBuildRecordHasTheVersionOfTheRules) {
    TestArea ta(TEST_NAME, true);
    const fs::path record = ta.getPath(CogBuildRecordFileName);
    EXPECT_FALSE(readCogBuildRecord(record).has_value());
    writeCogBuildRecord(record, CogBuildRecord{});
    const auto back = readCogBuildRecord(record);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->version, 1);
    const auto bytes = io::readAllBytes(record);
    EXPECT_EQ(json::parse(bytes.begin(), bytes.end()), (json{{"version", 1}}));
    io::writeFileAtomic(record, "not json", "test record");
    EXPECT_FALSE(readCogBuildRecord(record).has_value());
}

// The probe records the nodata of each band ("nan" for NaN) and whether the raster has a mask of its own
TEST(CogBuildPolicy, theIndexHasTheNodataOfTheBandsAndTheMaskOfTheRaster) {
    TestArea ta(TEST_NAME, true);
    std::vector<fs::path> inputs;
    for (const Source& s : kSources)
        inputs.push_back(writeSource(ta.getPath(std::string(s.name) + ".tif"), s));
    const json added = contract::makeDataset(ta.getFolder("dataset"), inputs);
    ASSERT_EQ(added.size(), kSources.size());

    for (const json& entry : added) {
        const std::string name = entry["path"].get<std::string>();
        const Source& s = *std::find_if(kSources.begin(), kSources.end(),
                                        [&](const Source& c) { return name == std::string(c.name) + ".tif"; });
        const json& properties = entry["properties"];
        EXPECT_EQ(properties["hasMask"], s.mask) << name;
        const json& bands = properties["bands"];
        ASSERT_EQ(bands.size(), static_cast<size_t>(s.bands)) << name;
        // A GeoTIFF has one nodata for every band, its alpha band too
        for (int b = 0; b < s.bands; b++) {
            if (!s.noData)
                EXPECT_FALSE(bands[b].contains("noData")) << name << " band " << b + 1;
            else if (std::isnan(*s.noData))
                EXPECT_EQ(bands[b]["noData"], "nan") << name;
            else
                EXPECT_EQ(bands[b]["noData"], *s.noData) << name;
        }
        EXPECT_EQ(bands[0]["colorInterp"] == "Palette", s.palette) << name;
        const auto facts = CogBuildPolicy::indexed(properties);
        ASSERT_TRUE(facts.has_value()) << name;
        EXPECT_EQ(CogBuildPolicy::outdates(*facts), s.outdated) << name;
    }
}

// A COG built now is complete; one built before (no build record) is complete unless its source is
// affected, whether the index holds the facts or not (an index of 2.7, read from the header of its
// COG); the next build builds the affected ones again
TEST(CogBuildPolicy, aCogBuiltBeforeTheRulesIsIncompleteOnlyWhenItsSourceIsAffected) {
    TestArea ta(TEST_NAME, true);
    EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");
    std::vector<fs::path> inputs;
    for (const Source& s : kSources)
        inputs.push_back(writeSource(ta.getPath(std::string(s.name) + ".tif"), s));
    const fs::path root = ta.getFolder("dataset");
    const json added = contract::makeDataset(root, inputs);
    const std::string r = root.string();
    ASSERT_EQ(DDBBuild(r.c_str(), "", nullptr, false, false), DDBERR_NONE) << DDBGetLastError();

    for (size_t i = 0; i < kSources.size(); i++) {
        const Source& s = kSources[i];
        const std::string path = std::string(s.name) + ".tif";
        ASSERT_EQ(added[i]["path"], path);
        const fs::path folder = root / ".ddb" / "build" / added[i]["hash"].get<std::string>() / CogBuildSubfolder;
        SCOPED_TRACE(path);

        // Built now: the record of the rules
        ASSERT_TRUE(fs::exists(folder / CogFileName));
        const auto record = readCogBuildRecord(folder / CogBuildRecordFileName);
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(record->version, CogBuildPolicy::kVersion);
        EXPECT_TRUE(reportedComplete(r, path));

        // Built before, indexed by this release: the index decides
        writeOldCog(inputs[i], folder / CogFileName);
        fs::remove(folder / CogBuildRecordFileName);
        EXPECT_EQ(reportedComplete(r, path), !s.outdated);
        // A record of an older version is no record of the rules
        io::writeFileAtomic(folder / CogBuildRecordFileName, R"({"version": 0})", "test record");
        EXPECT_EQ(reportedComplete(r, path), !s.outdated);
        fs::remove(folder / CogBuildRecordFileName);

        // Indexed by 2.7: the bands without their nodata, no hasMask; the COG it built says
        {
            const auto db = ddb::open(r, false);
            auto q = db->query(
                "UPDATE entries SET properties = json_remove(properties, '$.hasMask', '$.bands[0].noData', "
                "'$.bands[1].noData', '$.bands[2].noData', '$.bands[3].noData') WHERE path = ?");
            q->bind(1, path);
            q->execute();
        }
        EXPECT_EQ(reportedComplete(r, path), !s.outdated);
    }

    // The next build builds the outdated COGs again, and only them: the healthy ones have the record
    // of their check
    std::vector<fs::file_time_type> before;
    for (size_t i = 0; i < kSources.size(); i++)
        before.push_back(fs::last_write_time(root / ".ddb" / "build" / added[i]["hash"].get<std::string>() /
                                             CogBuildSubfolder / CogFileName));
    ASSERT_EQ(DDBBuild(r.c_str(), "", nullptr, false, false), DDBERR_NONE) << DDBGetLastError();
    for (size_t i = 0; i < kSources.size(); i++) {
        const Source& s = kSources[i];
        const fs::path folder = root / ".ddb" / "build" / added[i]["hash"].get<std::string>() / CogBuildSubfolder;
        SCOPED_TRACE(s.name);
        EXPECT_TRUE(reportedComplete(r, std::string(s.name) + ".tif"));
        const auto record = readCogBuildRecord(folder / CogBuildRecordFileName);
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(record->origin, s.outdated ? CogBuildRecord::Origin::Build : CogBuildRecord::Origin::Verified);
        EXPECT_EQ(fs::last_write_time(folder / CogFileName) != before[i], s.outdated);
    }
    DDBReleaseConnections(r.c_str(), nullptr);
}

// A COG holds its raster when its grid holds every corner of the footprint, within a few of its pixels;
// a pole of the footprint is taken at the limit of Web Mercator
TEST(CogBuildPolicy, misplacesACogWhoseGridDoesNotHoldEveryCornerOfTheFootprint) {
    // A raster of about 700 x 1100 m at 11 E, 45 N, its COG padded to a box of 2 km more
    const BasicPolygonGeometry footprint = footprintOf(11.0, 45.0, 11.01, 45.01);
    const BuiltCog cog = cogOver(10.99, 44.99, 11.02, 45.02);
    EXPECT_FALSE(CogBuildPolicy::misplaces(cog, footprint));
    // Nothing to compare
    EXPECT_FALSE(CogBuildPolicy::misplaces(cog, BasicPolygonGeometry()));

    // Moved 34 km west (the prime meridian of NTF Paris in grads, read wrongly)
    BuiltCog moved = cog;
    moved.extent[0] -= 34000;
    moved.extent[2] -= 34000;
    EXPECT_TRUE(CogBuildPolicy::misplaces(moved, footprint));

    // A grid that ends right at a corner holds it within the tolerance, not past it
    const auto ne = mercator(11.01, 45.01);
    BuiltCog tight = cog;
    tight.extent[2] = ne[0] - (CogBuildPolicy::kPlacementTolerancePixels - 0.5) * cog.pixelSize;
    EXPECT_FALSE(CogBuildPolicy::misplaces(tight, footprint));
    tight.extent[2] = ne[0] - (CogBuildPolicy::kPlacementTolerancePixels + 0.5) * cog.pixelSize;
    EXPECT_TRUE(CogBuildPolicy::misplaces(tight, footprint));

    // The whole world, up to the limit of Web Mercator
    const BuiltCog world = cogOver(-180, -85.0511287798066, 180, 85.0511287798066);
    EXPECT_FALSE(CogBuildPolicy::misplaces(world, footprintOf(-180, -90, 180, 90)));
    // A raster around the south pole (its footprint: every longitude from the pole to 60 S): a COG
    // that reaches the limit of Web Mercator holds it, one cut at the latitude of its edges does not
    EXPECT_FALSE(CogBuildPolicy::misplaces(cogOver(-180, -85.0511287798066, 180, -59.9),
                                           footprintOf(-180, -90, 180, -60)));
    EXPECT_TRUE(CogBuildPolicy::misplaces(cogOver(-180, -71.5, 180, -59.9), footprintOf(-180, -90, 180, -60)));

    // A COG without a CRS cannot hold the footprint
    BuiltCog noCrs = cog;
    noCrs.wkt.clear();
    EXPECT_THROW(CogBuildPolicy::misplaces(noCrs, footprint), AppException);
}

// A healthy COG built before the rules (no record) is built again when it lies elsewhere than its
// raster: moved 34 km, as a CRS read wrongly moved it. The COG of the builds before, on the tiles of
// EPSG:3857, and GDAL's COG in the CRS of the source both hold it where it is
TEST(CogBuildPolicy, aHealthyCogBuiltBeforeTheRulesIsIncompleteWhenItLiesElsewhereThanItsRaster) {
    TestArea ta(TEST_NAME, true);
    EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");
    std::vector<Source> healthy;
    std::copy_if(kSources.begin(), kSources.end(), std::back_inserter(healthy), [](const Source& s) { return !s.outdated; });
    std::vector<fs::path> inputs;
    for (const Source& s : healthy)
        inputs.push_back(writeSource(ta.getPath(std::string(s.name) + ".tif"), s));
    const fs::path root = ta.getFolder("dataset");
    const json added = contract::makeDataset(root, inputs);
    const std::string r = root.string();
    ASSERT_EQ(DDBBuild(r.c_str(), "", nullptr, false, false), DDBERR_NONE) << DDBGetLastError();

    std::vector<fs::path> folders;
    for (size_t i = 0; i < healthy.size(); i++) {
        const std::string path = std::string(healthy[i].name) + ".tif";
        ASSERT_EQ(added[i]["path"], path);
        const fs::path folder = root / ".ddb" / "build" / added[i]["hash"].get<std::string>() / CogBuildSubfolder;
        folders.push_back(folder);
        SCOPED_TRACE(path);
        fs::remove(folder / CogBuildRecordFileName);

        // In place: on the tiles of EPSG:3857, as the builds before warped it, or in the CRS of the source
        // (each check records it: the record is removed with each COG written here)
        writeOldGridCog(inputs[i], folder / CogFileName);
        EXPECT_TRUE(reportedComplete(r, path));
        fs::remove(folder / CogBuildRecordFileName);
        writeOldCog(inputs[i], folder / CogFileName);
        EXPECT_TRUE(reportedComplete(r, path));
        fs::remove(folder / CogBuildRecordFileName);

        // Moved 34 km east: no record, checked again each time
        writeOldGridCog(inputs[i], folder / CogFileName, "", 34000);
        EXPECT_FALSE(reportedComplete(r, path));
        EXPECT_FALSE(fs::exists(folder / CogBuildRecordFileName));
    }

    // The next build builds them again
    ASSERT_EQ(DDBBuild(r.c_str(), "", nullptr, false, false), DDBERR_NONE) << DDBGetLastError();
    for (size_t i = 0; i < healthy.size(); i++) {
        SCOPED_TRACE(healthy[i].name);
        EXPECT_TRUE(fs::exists(folders[i] / CogBuildRecordFileName));
        EXPECT_TRUE(reportedComplete(r, std::string(healthy[i].name) + ".tif"));
    }
    DDBReleaseConnections(r.c_str(), nullptr);
}

// GeoTIFFs whose CRS the builds before read wrongly, which put their COG away from the raster: NTF
// (Paris) in grads 34 km west, EPSG:2853 in US feet and EPSG:2838 with an IMAGINE citation in feet
// read in metres (thousands of kilometres). Their old COGs, on the tiles of EPSG:3857 from the CRS
// read wrongly, are incomplete; COGs from their own CRS are complete. The rasters have no nodata,
// mask or alpha band, whose COG is outdated anyway: their index says they have a mask, so that the
// placement alone decides. Files of GDAL's test data, when present
TEST(CogBuildPolicy, theCogOfAGeoTiffWhoseCrsTheBuildsBeforeReadWronglyIsIncomplete) {
    const fs::path data = "/mnt/data/datasets/orthos/geotiff-edge-cases/pangeo-example-tiff/gdal/gcore/data";
    struct Case {
        const char* file;
        std::string wrongSrs;  // the CRS read wrongly, else a shift east in metres
        double shiftX;
    };
    const std::vector<Case> cases = {{"test_gf.tif", "", -34000},
                                     {"epsg_2853_with_us_feet.tif", "EPSG:2853", 0},
                                     {"citation_mixedcase.tif", "EPSG:2838", 0}};
    for (const Case& c : cases)
        if (!fs::exists(data / c.file))
            GTEST_SKIP() << (data / c.file).string() << " is not present";

    TestArea ta(TEST_NAME, true);
    EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");
    std::vector<fs::path> inputs;
    for (const Case& c : cases) {
        inputs.push_back(ta.getPath(c.file));
        fs::copy_file(data / c.file, inputs.back(), fs::copy_options::overwrite_existing);
    }
    const fs::path root = ta.getFolder("dataset");
    const json added = contract::makeDataset(root, inputs);
    const std::string r = root.string();
    ASSERT_EQ(DDBBuild(r.c_str(), "", nullptr, false, false), DDBERR_NONE) << DDBGetLastError();
    {
        const auto db = ddb::open(r, false);
        db->query("UPDATE entries SET properties = json_set(properties, '$.hasMask', json('true'))")->execute();
    }

    for (size_t i = 0; i < cases.size(); i++) {
        const Case& c = cases[i];
        ASSERT_EQ(added[i]["path"], c.file);
        ASSERT_EQ(added[i]["type"], static_cast<int>(EntryType::GeoRaster));
        const fs::path folder = root / ".ddb" / "build" / added[i]["hash"].get<std::string>() / CogBuildSubfolder;
        SCOPED_TRACE(c.file);
        EXPECT_TRUE(reportedComplete(r, c.file));

        fs::remove(folder / CogBuildRecordFileName);
        writeOldGridCog(inputs[i], folder / CogFileName);
        EXPECT_TRUE(reportedComplete(r, c.file));
        fs::remove(folder / CogBuildRecordFileName);
        writeOldGridCog(inputs[i], folder / CogFileName, c.wrongSrs, c.shiftX);
        EXPECT_FALSE(reportedComplete(r, c.file));
    }
    DDBReleaseConnections(r.c_str(), nullptr);
}

// A raster around a pole: its COG built now reaches the limit of Web Mercator at that pole over every
// longitude and holds it; the COG of a build that stopped at the latitude of its edges does not
TEST(CogBuildPolicy, theCogOfARasterAroundAPoleHoldsItWhenItReachesTheLimitOfWebMercator) {
    TestArea ta(TEST_NAME, true);
    EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");
    // RGBA in EPSG:3031, 6400 x 4800 km round the south pole
    const Source polar{"polar-rgba", 4, GDT_Byte, true, std::nullopt, false, false, false, 3031, -1500000.0, 1500000.0,
                       100000.0};
    const fs::path input = writeSource(ta.getPath("polar-rgba.tif"), polar);
    const fs::path root = ta.getFolder("dataset");
    const json added = contract::makeDataset(root, {input});
    const std::string r = root.string();
    ASSERT_EQ(added[0]["polygon_geom"]["geometry"]["coordinates"][0][2][1], -90.0) << added[0].dump();
    ASSERT_EQ(DDBBuild(r.c_str(), "", nullptr, false, false), DDBERR_NONE) << DDBGetLastError();
    const fs::path folder = root / ".ddb" / "build" / added[0]["hash"].get<std::string>() / CogBuildSubfolder;

    // Without its record, the COG built now holds the raster
    fs::remove(folder / CogBuildRecordFileName);
    EXPECT_TRUE(reportedComplete(r, "polar-rgba.tif"));

    // Its northern half: cut short of the pole
    const fs::path half = ta.getPath("half.tif");
    {
        auto cog = adoptDataset(GDALOpen(io::toUtf8(folder / CogFileName).c_str(), GA_ReadOnly));
        ASSERT_TRUE(cog);
        const int w = GDALGetRasterXSize(datasetHandle(cog)), h = GDALGetRasterYSize(datasetHandle(cog));
        CPLStringList list = cslFromStrings({"-of", "COG", "-srcwin", "0", "0", std::to_string(w), std::to_string(h / 2)});
        const GdalOptionsPtr<GDALTranslateOptions> options(GDALTranslateOptionsNew(list.List(), nullptr));
        ASSERT_TRUE(options);
        auto out = adoptDataset(GDALTranslate(io::toUtf8(half).c_str(), datasetHandle(cog), options.get(), nullptr));
        ASSERT_TRUE(out);
    }
    fs::remove(folder / CogBuildRecordFileName);
    fs::copy_file(half, folder / CogFileName, fs::copy_options::overwrite_existing);
    EXPECT_FALSE(reportedComplete(r, "polar-rgba.tif"));
    DDBReleaseConnections(r.c_str(), nullptr);
}

// A COG built before the rules that its check finds healthy and in its place gets a record of that
// check ({"version", "origin": "verified"}), written under the build lock of its folder: the checks
// after it read the record alone, not the header of the COG (here a file that is no TIFF any more,
// which a read would refuse). No record is written while a build holds the lock, in a folder that
// cannot be written, or in a folder that is not the entry's own build folder; the check gives its
// answer all the same
TEST(CogBuildPolicy, aCogFoundInItsPlaceIsRecordedSoItsHeaderIsReadOnce) {
    TestArea ta(TEST_NAME, true);
    EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");
    const Source& rgba = *std::find_if(kSources.begin(), kSources.end(),
                                       [](const Source& s) { return std::string(s.name) == "rgba"; });
    const fs::path input = writeSource(ta.getPath("rgba.tif"), rgba);
    const fs::path root = ta.getFolder("dataset");
    const json added = contract::makeDataset(root, {input});
    const std::string r = root.string();
    ASSERT_EQ(DDBBuild(r.c_str(), "", nullptr, false, false), DDBERR_NONE) << DDBGetLastError();
    const fs::path base = root / ".ddb" / "build" / added[0]["hash"].get<std::string>();
    const fs::path folder = base / CogBuildSubfolder;
    const fs::path record = folder / CogBuildRecordFileName;
    const auto entry = getEntry(ddb::open(r, false).get(), "rgba.tif");
    ASSERT_TRUE(entry.has_value());
    const EntryHandler& handler = entryHandler(EntryType::GeoRaster);

    // The COG of a build before the rules
    writeOldGridCog(input, folder / CogFileName);
    fs::remove(record);

    // While a build holds the lock: complete, nothing written
    {
        const BuildLock building(io::toUtf8(folder));
        EXPECT_TRUE(handler.isComplete(*entry, base));
        EXPECT_FALSE(fs::exists(record));
    }

    // In a folder that is not the entry's own build folder: complete, nothing written
    const fs::path other = ta.getFolder("other");
    fs::create_directories(other / CogBuildSubfolder);
    fs::copy_file(folder / CogFileName, other / CogBuildSubfolder / CogFileName);
    EXPECT_TRUE(handler.isComplete(*entry, other));
    EXPECT_FALSE(fs::exists(other / CogBuildSubfolder / CogBuildRecordFileName));

#ifndef WIN32
    // In a folder that cannot be written (a read-only dataset): complete, nothing written
    if (geteuid() != 0) {
        fs::permissions(base, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        fs::perm_options::remove);
        fs::permissions(folder, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        fs::perm_options::remove);
        EXPECT_TRUE(reportedComplete(r, "rgba.tif"));
        EXPECT_FALSE(fs::exists(record));
        fs::permissions(base, fs::perms::owner_write, fs::perm_options::add);
        fs::permissions(folder, fs::perms::owner_write, fs::perm_options::add);
    }
#endif

    // The first check reads the header and records it
    EXPECT_TRUE(reportedComplete(r, "rgba.tif"));
    const auto written = readCogBuildRecord(record);
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(written->version, CogBuildPolicy::kVersion);
    EXPECT_EQ(written->origin, CogBuildRecord::Origin::Verified);
    const auto bytes = io::readAllBytes(record);
    EXPECT_EQ(json::parse(bytes.begin(), bytes.end()), (json{{"version", 1}, {"origin", "verified"}}));
    EXPECT_FALSE(fs::exists(folder.string() + ".building"));

    // The checks after it read the record alone: a header read would refuse this file
    DDBReleaseConnections(r.c_str(), nullptr);
    io::writeFileAtomic(folder / CogFileName, "no TIFF", "test COG");
    EXPECT_TRUE(reportedComplete(r, "rgba.tif"));
    EXPECT_TRUE(handler.isComplete(*entry, base));
    DDBReleaseConnections(r.c_str(), nullptr);
}
