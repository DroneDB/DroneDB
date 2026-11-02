/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDBExportSplatSpz: the splat of an entry as one .spz file for other tools, kept in the build
// folder. It is an export new in 2.0, so an invalid argument is DDBERR_INVALIDARGS, an entry that is
// not in the dataset or not built DDBERR_NOTFOUND and a build (or an export) that holds the lock
// of the entry DDBERR_BUILDINPROGRESS; the output is written on DDBERR_NONE only. What a build does
// to the file is the matter of gsplat_test.cpp.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "build_layout.h"
#include "buildlock.h"
#include "ddb.h"
#include "gsplat.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    const std::string H_SPLAT(64, 'a');
    constexpr int kSplats = 40;

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

    // An antimatter15 .splat of n splats: 32 bytes each
    void writeSplatBinary(const fs::path &path, int n)
    {
        std::ofstream out(path.string(), std::ios::binary);
        for (int i = 0; i < n; ++i)
        {
            const float pos[3] = {static_cast<float>(i % 10), static_cast<float>(i / 10), 0.0f};
            const float scale[3] = {0.01f, 0.01f, 0.01f};
            const uint8_t rgba[4] = {128, 130, 132, 255};
            const uint8_t rot[4] = {255, 128, 128, 128};
            out.write(reinterpret_cast<const char *>(pos), sizeof(pos));
            out.write(reinterpret_cast<const char *>(scale), sizeof(scale));
            out.write(reinterpret_cast<const char *>(rgba), sizeof(rgba));
            out.write(reinterpret_cast<const char *>(rot), sizeof(rot));
        }
    }

    // A dataset with scene.splat, a splat entry whose build (model.rad) is there unless unbuilt, and
    // a.txt
    struct Dataset
    {
        IndexFixture index;
        std::string root;
        fs::path build;
        fs::path gsplat;
        fs::path exported;

        explicit Dataset(const std::string &name, bool built = true)
            : index(name), root(index.root.string()), build(fs::absolute(index.db->buildDirectory()) / H_SPLAT),
              gsplat(build / GsplatBuildSubfolder), exported(gsplat / GsplatExportFileName)
        {
            writeSplatBinary(index.root / "scene.splat", kSplats);
            insertEntry(index.db.get(), "scene.splat", H_SPLAT, EntryType::GaussianSplat);
            insertEntry(index.db.get(), "a.txt", std::string(64, 'c'), EntryType::Generic);
            writeFile(index.root / "a.txt", "alpha");
            if (built)
                writeFile(gsplat / GsplatRadFileName, "x");
        }

        ~Dataset() { DDBReleaseConnections(root.c_str(), nullptr); }

        DDBErr exportSpz(const char *path, json &result) const
        {
            char *output = nullptr;
            const DDBErr code = DDBExportSplatSpz(root.c_str(), path, &output);
            result = output != nullptr ? json::parse(output) : json();
            DDBFree(output);
            return code;
        }
    };

    TEST(CAbiSplatExport, convertsTheSplatIntoTheBuildFolder)
    {
        Dataset d(TEST_NAME);
        json result;
        ASSERT_DDB_OK(d.exportSpz("scene.splat", result));

        EXPECT_EQ(result["role"], "gsplat.export");
        EXPECT_EQ(result["cached"], false);
        EXPECT_EQ(result["path"], io::toUtf8(d.exported));
        ASSERT_TRUE(fs::is_regular_file(d.exported));

        GaussianSplatInfo info;
        ASSERT_TRUE(getGaussianSplatInfo(io::toUtf8(d.exported), info));
        EXPECT_EQ(info.splatCount, static_cast<uint64_t>(kSplats));

        // Nothing else was written into the dataset or the build
        EXPECT_FALSE(fs::exists(d.gsplat / GsplatFileName));
        EXPECT_FALSE(fs::exists(d.gsplat / GsplatBoundsFileName));
        EXPECT_FALSE(fs::exists(io::concat(d.gsplat, BuildLockSuffix)));
        EXPECT_FALSE(fs::exists(d.index.root / "export.spz"));
    }

    TEST(CAbiSplatExport, aSecondCallFindsTheFileAndLeavesItAlone)
    {
        Dataset d(TEST_NAME);
        json result;
        ASSERT_DDB_OK(d.exportSpz("scene.splat", result));
        ASSERT_EQ(result["cached"], false);
        const auto madeAt = fs::last_write_time(d.exported);
        const auto size = fs::file_size(d.exported);

        ASSERT_DDB_OK(d.exportSpz("scene.splat", result));
        EXPECT_EQ(result["cached"], true);
        EXPECT_EQ(result["path"], io::toUtf8(d.exported));
        EXPECT_EQ(fs::last_write_time(d.exported), madeAt);
        EXPECT_EQ(fs::file_size(d.exported), size);
    }

    TEST(CAbiSplatExport, aFileOlderThanTheDatasetFileOrEmptyIsMadeAgain)
    {
        Dataset d(TEST_NAME);
        json result;
        ASSERT_DDB_OK(d.exportSpz("scene.splat", result));

        // The dataset file changed after the export was made
        fs::last_write_time(d.exported, fs::last_write_time(d.index.root / "scene.splat") - std::chrono::hours(1));
        ASSERT_DDB_OK(d.exportSpz("scene.splat", result));
        EXPECT_EQ(result["cached"], false);
        EXPECT_GT(fs::last_write_time(d.exported), fs::last_write_time(d.index.root / "scene.splat") - std::chrono::seconds(1));

        // A file with nothing in it is not an export
        { std::ofstream(d.exported, std::ios::binary | std::ios::trunc); }
        ASSERT_DDB_OK(d.exportSpz("scene.splat", result));
        EXPECT_EQ(result["cached"], false);
        EXPECT_GT(fs::file_size(d.exported), 0u);
    }

    TEST(CAbiSplatExport, theExportIsAnArtifactOfTheEntryOnceItExists)
    {
        Dataset d(TEST_NAME);
        const char *paths[] = {"scene.splat"};
        const auto artifacts = [&] {
            char *out = nullptr;
            EXPECT_EQ(DDBGetBuildArtifacts(d.root.c_str(), paths, 1, &out), DDBERR_NONE) << DDBGetLastError();
            const json found = json::parse(out);
            DDBFree(out);
            return found[0]["build"];
        };

        EXPECT_EQ(artifacts()["artifacts"].size(), 1u);

        json result;
        ASSERT_DDB_OK(d.exportSpz("scene.splat", result));
        const json build = artifacts();
        ASSERT_EQ(build["artifacts"].size(), 2u);
        EXPECT_EQ(build["artifacts"][0]["role"], "gsplat.lod");
        EXPECT_EQ(build["artifacts"][1]["role"], "gsplat.export");
        EXPECT_EQ(build["artifacts"][1]["path"], "gsplat/export.spz");
        EXPECT_EQ(build["artifacts"][1]["downloadName"], "export.spz");
        EXPECT_EQ(build["artifacts"][1]["mediaType"], "application/octet-stream");
        EXPECT_FALSE(build["artifacts"][1].contains("legacy"));
        EXPECT_EQ(build["state"], "complete");
    }

    TEST(CAbiSplatExport, refusesAnEntryThatIsNotBuilt)
    {
        {
            // No build folder at all
            Dataset d(std::string(TEST_NAME) + "-none", false);
            json result;
            EXPECT_EQ(d.exportSpz("scene.splat", result), DDBERR_NOTFOUND);
            EXPECT_NE(std::string(DDBGetLastError()).find("not built"), std::string::npos) << DDBGetLastError();
            EXPECT_FALSE(fs::exists(d.gsplat));
            EXPECT_FALSE(fs::exists(io::concat(d.gsplat, BuildLockSuffix)));
        }
        {
            // A build folder without the model: an interrupted build, or an export only
            Dataset d(std::string(TEST_NAME) + "-partial", false);
            writeFile(d.gsplat / GsplatBoundsFileName, "{}");
            json result;
            EXPECT_EQ(d.exportSpz("scene.splat", result), DDBERR_NOTFOUND);
            EXPECT_FALSE(fs::exists(d.exported));
        }
    }

    TEST(CAbiSplatExport, theLockOfTheEntryExcludesTheExport)
    {
        Dataset d(TEST_NAME);
        json result;
        {
            // A build that runs holds the lock of gsplat/ until it has published the folder
            BuildLock running(io::toUtf8(d.gsplat));
            EXPECT_EQ(d.exportSpz("scene.splat", result), DDBERR_BUILDINPROGRESS);
            EXPECT_FALSE(fs::exists(d.exported));
        }
        ASSERT_DDB_OK(d.exportSpz("scene.splat", result));
        EXPECT_TRUE(fs::exists(d.exported));

        // The lock is released with the call: a build can take it
        EXPECT_NO_THROW(BuildLock(io::toUtf8(d.gsplat)));
    }

    TEST(CAbiSplatExport, refusesWhatIsNotASplatOrNotConverted)
    {
        Dataset d(TEST_NAME);
        json result;
        EXPECT_EQ(d.exportSpz("a.txt", result), DDBERR_INVALIDARGS);
        EXPECT_EQ(d.exportSpz("missing.splat", result), DDBERR_NOTFOUND);

        // KSPLAT, SOG and RAD are not converted to SPZ: refused before any lock is taken
        for (const char *name : {"scene.ksplat", "scene.sog", "scene.rad"})
        {
            SCOPED_TRACE(name);
            const std::string hash(64, name[6] == 'k' ? 'd' : name[6] == 's' ? 'e' : 'f');
            writeFile(d.index.root / name, "x");
            insertEntry(d.index.db.get(), name, hash, EntryType::GaussianSplat);
            writeFile(fs::absolute(d.index.db->buildDirectory()) / hash / "gsplat" / "model.rad", "x");
            EXPECT_EQ(d.exportSpz(name, result), DDBERR_INVALIDARGS);
            EXPECT_NE(std::string(DDBGetLastError()).find("only"), std::string::npos) << DDBGetLastError();
            EXPECT_FALSE(fs::exists(fs::absolute(d.index.db->buildDirectory()) / hash / "gsplat" / "export.spz"));
        }

        // The file of the entry is gone from the dataset
        fs::remove(d.index.root / "scene.splat");
        EXPECT_EQ(d.exportSpz("scene.splat", result), DDBERR_EXCEPTION);
        EXPECT_NE(std::string(DDBGetLastError()).find("does not exist"), std::string::npos) << DDBGetLastError();
    }

    TEST(CAbiSplatExport, aSourceThatCannotBeDecodedFailsAndLeavesNoFile)
    {
        Dataset d(TEST_NAME);
        writeFile(d.index.root / "scene.splat", "not a splat");
        json result;
        EXPECT_EQ(d.exportSpz("scene.splat", result), DDBERR_EXCEPTION);
        EXPECT_FALSE(fs::exists(d.exported));
        EXPECT_FALSE(fs::exists(io::concat(d.gsplat, BuildLockSuffix)));
    }

    TEST(CAbiSplatExport, checksItsArguments)
    {
        Dataset d(TEST_NAME);
        char *out = nullptr;
        EXPECT_EQ(DDBExportSplatSpz(nullptr, "scene.splat", &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBExportSplatSpz(" ", "scene.splat", &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBExportSplatSpz(d.root.c_str(), nullptr, &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBExportSplatSpz(d.root.c_str(), "", &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBExportSplatSpz(d.root.c_str(), "../scene.splat", &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBExportSplatSpz(d.root.c_str(), "scene.splat", nullptr), DDBERR_INVALIDARGS);
        EXPECT_EQ(out, nullptr);
        EXPECT_FALSE(fs::exists(d.exported));
    }
}  // namespace
