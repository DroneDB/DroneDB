/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The COG of a GeoRaster built before the rules of CogBuildPolicy is built again only when the COG
// built now differs from it: for a source without nodata, mask or alpha band, with the nodata of
// several bands, or with a colour table. The probe records the nodata of the bands and the mask of
// the raster, so the index decides; for an entry indexed before, the header of the COG it has does.
// Every API that reports the state of a build agrees (DDBIsBuildComplete, the manifest the
// completeness sweep of Registry reads, the listing with builds, the status).

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "build_layout.h"
#include "cog_build_policy.h"
#include "contract_fixtures.h"
#include "cpl_conv.h"
#include "ddb.h"
#include "dbops.h"
#include "gdal_raii.h"
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

// A GeoTIFF in UTM 32N (its COG is warped to the tiles of EPSG:3857) with the transparency of `s`
fs::path writeSource(const fs::path& path, const Source& s) {
    CPLSetThreadLocalConfigOption("GDAL_TIFF_INTERNAL_MASK", "YES");
    {
        auto ds = adoptDataset(GDALCreate(GDALGetDriverByName("GTiff"), io::toUtf8(path).c_str(), kWidth, kHeight,
                                          s.bands, s.type, nullptr));
        EXPECT_TRUE(ds);
        GDALDatasetH h = datasetHandle(ds);
        double gt[6] = {500000.0, 2.0, 0.0, 5000000.0, 0.0, -2.0};
        GDALSetGeoTransform(h, gt);
        SrsPtr srs(OSRNewSpatialReference(nullptr));
        OSRImportFromEPSG(srs.get(), 32632);
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

    // The next build builds the outdated COGs again, and only them
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
        EXPECT_EQ(fs::exists(folder / CogBuildRecordFileName), s.outdated);
        EXPECT_EQ(fs::last_write_time(folder / CogFileName) != before[i], s.outdated);
    }
    DDBReleaseConnections(r.c_str(), nullptr);
}
