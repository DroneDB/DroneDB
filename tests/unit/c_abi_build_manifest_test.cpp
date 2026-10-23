/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// XI-04: the C ABI of the build manifest (DDBGetEntryTypes, DDBGetBuildArtifacts,
// DDBResolveBuildFile, DDBCopyBuildArtifacts, DDBListEntriesEx). They are ABI 1.1 exports, so an
// invalid argument is DDBERR_INVALIDARGS, a missing entry or file DDBERR_NOTFOUND, a running
// build DDBERR_BUILDINPROGRESS and a full disk DDBERR_RETRYABLE; outputs are written on
// DDBERR_NONE only. The JSON shapes are pinned by the contract fixtures (contract_test.cpp).

#include <cstring>
#include <string>
#include <vector>

#include "build_layout.h"
#include "buildlock.h"
#include "ddb.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    const std::string H_COG(64, 'a');
    const std::string H_PENDING(64, 'b');

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

    // Expects the call to fail with `code` and a message; the output stays untouched
#define EXPECT_DDB_FAILS(code, call, out)                                    \
    do                                                                       \
    {                                                                        \
        char *sentinel = reinterpret_cast<char *>(&out);                     \
        out = sentinel;                                                      \
        EXPECT_EQ((call), code) << #call << ": " << DDBGetLastError();       \
        EXPECT_STRNE(DDBGetLastError(), "") << #call;                        \
        EXPECT_EQ(out, sentinel) << #call << " wrote its output on failure"; \
        out = nullptr;                                                       \
    } while (0)

    std::string take(char *s)
    {
        std::string copy = s != nullptr ? s : "";
        DDBFree(s);
        return copy;
    }

    // A dataset with ortho.tif (built: cog/cog.tif), pending.shp (deferred) and a.txt
    struct Dataset
    {
        IndexFixture index;
        std::string root;
        fs::path build;

        explicit Dataset(const std::string &name, const fs::path &folder = "ds", bool recreate = true)
            : index(name, folder, recreate), root(index.root.string()), build(fs::absolute(index.db->buildDirectory()))
        {
            insertEntry(index.db.get(), "ortho.tif", H_COG, EntryType::GeoRaster);
            insertEntry(index.db.get(), "pending.shp", H_PENDING, EntryType::Vector);
            insertEntry(index.db.get(), "a.txt", std::string(64, 'c'), EntryType::Generic);
        }

        ~Dataset() { DDBReleaseConnections(root.c_str(), nullptr); }

        void buildCog() const { writeFile(build / H_COG / "cog" / "cog.tif", "x"); }
    };

    TEST(CAbiBuildManifest, GetEntryTypes)
    {
        char *out = nullptr;
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBGetEntryTypes(nullptr), out);

        ASSERT_DDB_OK(DDBGetEntryTypes(&out));
        const json types = json::parse(take(out));
        ASSERT_TRUE(types.is_array());
        EXPECT_GT(types.size(), 10u);
        EXPECT_EQ(types[0]["id"], 0);
    }

    TEST(CAbiBuildManifest, GetBuildArtifacts)
    {
        Dataset ds(TEST_NAME);
        ds.buildCog();
        const char *paths[] = {"ortho.tif", "a.txt", "missing.tif"};
        const char *withNull[] = {"ortho.tif", nullptr};
        char *out = nullptr;

        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBGetBuildArtifacts(nullptr, paths, 3, &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBGetBuildArtifacts(" ", paths, 3, &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBGetBuildArtifacts(ds.root.c_str(), nullptr, 1, &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBGetBuildArtifacts(ds.root.c_str(), paths, -1, &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBGetBuildArtifacts(ds.root.c_str(), withNull, 2, &out), out);
        EXPECT_EQ(DDBGetBuildArtifacts(ds.root.c_str(), paths, 3, nullptr), DDBERR_INVALIDARGS);

        ASSERT_DDB_OK(DDBGetBuildArtifacts(ds.root.c_str(), paths, 0, &out));
        EXPECT_EQ(take(out), "[]");

        ASSERT_DDB_OK(DDBGetBuildArtifacts(ds.root.c_str(), paths, 3, &out));
        const json j = json::parse(take(out));
        ASSERT_EQ(j.size(), 3u);
        EXPECT_EQ(j[0]["build"]["state"], "complete");
        EXPECT_EQ(j[0]["build"]["artifacts"][0]["role"], "raster.cog");
        EXPECT_EQ(j[1]["build"]["buildable"], false);
        EXPECT_EQ(j[2]["exists"], false);
    }

    TEST(CAbiBuildManifest, ResolveBuildFile)
    {
        Dataset ds(TEST_NAME);
        ds.buildCog();
        const char *r = ds.root.c_str();
        char *out = nullptr;

        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBResolveBuildFile(nullptr, H_COG.c_str(), "cog/cog.tif", &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBResolveBuildFile(r, nullptr, "cog/cog.tif", &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBResolveBuildFile(r, H_COG.c_str(), nullptr, &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBResolveBuildFile(r, "1234", "cog/cog.tif", &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBResolveBuildFile(r, H_COG.c_str(), "../../ds/a.txt", &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBResolveBuildFile(r, H_COG.c_str(), "/etc/passwd", &out), out);
        EXPECT_EQ(DDBResolveBuildFile(r, H_COG.c_str(), "cog/cog.tif", nullptr), DDBERR_INVALIDARGS);

        EXPECT_DDB_FAILS(DDBERR_NOTFOUND, DDBResolveBuildFile(r, H_COG.c_str(), "cog.building", &out), out);
        EXPECT_DDB_FAILS(DDBERR_NOTFOUND, DDBResolveBuildFile(r, H_COG.c_str(), "cog/x.tif", &out), out);
        EXPECT_DDB_FAILS(DDBERR_NOTFOUND, DDBResolveBuildFile(r, std::string(64, '0').c_str(), "cog/cog.tif", &out),
                         out);

        ASSERT_DDB_OK(DDBResolveBuildFile(r, H_COG.c_str(), "cog/cog.tif", &out));
        EXPECT_EQ(io::fromUtf8(take(out)), ds.build / H_COG / "cog" / "cog.tif");
    }

    TEST(CAbiBuildManifest, CopyBuildArtifacts)
    {
        EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");
        Dataset src(TEST_NAME);
        Dataset dst(TEST_NAME, "dst", false);
        src.buildCog();
        writeFile(src.build / (H_PENDING + ".pending"), "1700000000\npending.shx\n");
        const char *s = src.root.c_str();
        const char *d = dst.root.c_str();
        char *out = nullptr;

        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBCopyBuildArtifacts(nullptr, d, H_COG.c_str(), &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBCopyBuildArtifacts(s, "", H_COG.c_str(), &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBCopyBuildArtifacts(s, d, nullptr, &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBCopyBuildArtifacts(s, d, "xyz", &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBCopyBuildArtifacts(s, s, H_COG.c_str(), &out), out);
        EXPECT_EQ(DDBCopyBuildArtifacts(s, d, H_COG.c_str(), nullptr), DDBERR_INVALIDARGS);
        EXPECT_DDB_FAILS(DDBERR_NOTFOUND, DDBCopyBuildArtifacts(s, d, std::string(64, '0').c_str(), &out), out);
        {
            BuildLock running((src.build / H_COG / "cog").string());
            EXPECT_DDB_FAILS(DDBERR_BUILDINPROGRESS, DDBCopyBuildArtifacts(s, d, H_COG.c_str(), &out), out);
        }
        {
            EnvVarGuard floor("DDB_MIN_FREE_DISK_MB", "999999999999");
            EXPECT_DDB_FAILS(DDBERR_RETRYABLE, DDBCopyBuildArtifacts(s, d, H_COG.c_str(), &out), out);
            EXPECT_STREQ(DDBGetLastErrorReason(), "lowDiskSpace");
        }
        {
            // The DDBConfigure floor wins over DDB_MIN_FREE_DISK_MB=0 (DQ-15)
            const ScopedProcessConfig floor(R"({"minFreeDiskMb": 10000000000000})");
            EXPECT_DDB_FAILS(DDBERR_RETRYABLE, DDBCopyBuildArtifacts(s, d, H_COG.c_str(), &out), out);
            EXPECT_STREQ(DDBGetLastErrorReason(), "lowDiskSpace");
        }

        ASSERT_DDB_OK(DDBCopyBuildArtifacts(s, d, H_COG.c_str(), &out));
        const json copied = json::parse(take(out));
        EXPECT_EQ(copied["artifacts"][0]["role"], "raster.cog");
        EXPECT_TRUE(fs::exists(dst.build / H_COG / "cog" / "cog.tif"));

        ASSERT_DDB_OK(DDBCopyBuildArtifacts(s, d, H_PENDING.c_str(), &out));
        EXPECT_EQ(json::parse(take(out))["pending"], true);
        const char *paths[] = {"pending.shp"};
        ASSERT_DDB_OK(DDBGetBuildArtifacts(d, paths, 1, &out));
        const json state = json::parse(take(out));
        EXPECT_EQ(state[0]["build"]["state"], "pending");
        EXPECT_EQ(state[0]["build"]["lastAttempt"], 1700000000);
        EXPECT_EQ(state[0]["build"]["retryAfter"], 1700000060) << "the copied marker keeps its retry time";
    }

    // The options struct (XI-03): structSize first, at least the ABI 1.1 size; a newer caller's
    // unknown fields must be zero
    TEST(CAbiBuildManifest, ListEntriesEx)
    {
        Dataset ds(TEST_NAME);
        ds.buildCog();
        const char *r = ds.root.c_str();
        char *out = nullptr;

        DDBListEntriesOptions options;
        std::memset(&options, 0, sizeof(options));
        options.structSize = sizeof(DDBListEntriesOptions);
        options.recursive = true;
        options.withDetails = false;
        options.withBuild = false;

        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBListEntriesEx(nullptr, "", "", "", &options, &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBListEntriesEx(r, "", "", "", nullptr, &out), out);
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBListEntriesEx(r, "", "", "nosuchtype", &options, &out), out);
        EXPECT_EQ(DDBListEntriesEx(r, "", "", "", &options, nullptr), DDBERR_INVALIDARGS);
        DDBListEntriesOptions tooSmall = options;
        tooSmall.structSize = 4;
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBListEntriesEx(r, "", "", "", &tooSmall, &out), out);
        // The padding byte is reserved: a later ABI may put a 1-byte field there, which this
        // library must refuse rather than ignore
        DDBListEntriesOptions reservedSet = options;
        reservedSet.reserved = 1;
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBListEntriesEx(r, "", "", "", &reservedSet, &out), out);

        // A struct of a later ABI: accepted while its unknown fields are zero
        struct Later
        {
            DDBListEntriesOptions known;
            uint64_t unknown;
        } later{};
        later.known = options;
        later.known.structSize = sizeof(Later);
        ASSERT_DDB_OK(DDBListEntriesEx(r, "", "", "", &later.known, &out));
        take(out);
        later.unknown = 1;
        EXPECT_DDB_FAILS(DDBERR_INVALIDARGS, DDBListEntriesEx(r, "", "", "", &later.known, &out), out);

        // Without builds: DDBListEntries, byte for byte
        for (const bool details : {false, true})
        {
            options.withDetails = details;
            ASSERT_DDB_OK(DDBListEntriesEx(r, NULL, NULL, NULL, &options, &out));
            const std::string ex = take(out);
            ASSERT_DDB_OK(DDBListEntries(r, "", true, "", "", details, &out));
            EXPECT_EQ(ex, take(out));
        }

        options.withDetails = false;
        options.withBuild = true;
        ASSERT_DDB_OK(DDBListEntriesEx(r, "", "*.tif", "georaster", &options, &out));
        const json listed = json::parse(take(out));
        ASSERT_EQ(listed.size(), 1u);
        EXPECT_EQ(listed[0]["path"], "ortho.tif");
        EXPECT_EQ(listed[0]["build"]["state"], "complete");
        EXPECT_EQ(listed[0]["build"]["artifacts"][0]["path"], "cog/cog.tif");
    }
}
