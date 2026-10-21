/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-03: C ABI tests for the exports of ddb.h that had no direct test. They pin the behaviour
// of today's ABI (1.0), offline, with inputs made by the test:
//
//  - null and invalid arguments return DDBERR_EXCEPTION and set a non-empty DDBGetLastError(),
//    without crashing;
//  - a minimal call succeeds (a fresh index, a small GeoTIFF), and every output is released
//    with the function the header names (DDBFree for strings, DDBVSIFree for buffers).
//
// c_abi.ExportsHaveTests (tests/c_abi_coverage.cmake) fails when an export of ddb.h is not called
// by any test.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "ddb.h"
#include "gdal_inc.h"
#include "gdaltiler.h"
#include "gtest/gtest.h"
#include "json.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;
    using testutils::GeoRasterSpec;

    // Expects the call to fail with DDBERR_EXCEPTION and a message in DDBGetLastError()
#define EXPECT_DDB_INVALID(call)                                       \
    do                                                                 \
    {                                                                  \
        DDBSetLastError("");                                           \
        EXPECT_EQ((call), DDBERR_EXCEPTION) << #call;                  \
        EXPECT_STRNE(DDBGetLastError(), "") << #call;                  \
    } while (0)

    // Expects the call to succeed, printing the last error otherwise
#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

    // Copies a string returned by the library and frees it with DDBFree
    std::string take(char *s)
    {
        std::string copy = s != nullptr ? s : "";
        EXPECT_EQ(DDBFree(s), DDBERR_NONE);
        return copy;
    }

    // Copies a buffer returned by the library and frees it with DDBVSIFree
    std::vector<uint8_t> takeBuffer(uint8_t *buffer, int size)
    {
        std::vector<uint8_t> copy;
        if (buffer != nullptr && size > 0)
            copy.assign(buffer, buffer + size);
        EXPECT_EQ(DDBVSIFree(buffer), DDBERR_NONE);
        return copy;
    }

    // Deterministic broadband value in [0, 1): texture that alignment can lock on
    double noise(int x, int y)
    {
        uint32_t h = static_cast<uint32_t>(y) * 73856093u ^ static_cast<uint32_t>(x) * 19349663u;
        h ^= h >> 13;
        h *= 0x85ebca6bu;
        h ^= h >> 16;
        return static_cast<double>(h & 0xFFFFFFu) / static_cast<double>(0x1000000u);
    }

    // A small textured 8-bit GeoTIFF (EPSG:3857)
    fs::path smallRaster(const fs::path &path, int bands = 3, int size = 128)
    {
        GeoRasterSpec spec;
        spec.width = size;
        spec.height = size;
        spec.bands = bands;
        spec.value = [](int band, int x, int y) { return 20.0 + 200.0 * noise(x + band * 7, y); };
        return testutils::createGeoRaster(path, spec);
    }

    // An index with a.txt ("alpha") added; the fixture's connection stays open
    struct Dataset : IndexFixture
    {
        explicit Dataset(const std::string &name) : IndexFixture(name)
        {
            add("a.txt", "alpha");
        }

        void add(const std::string &rel, const std::string &content)
        {
            writeFile(root / rel, content);
            addToIndex(db.get(), {(root / rel).string()});
        }

        std::string path() const { return root.string(); }
    };

    class CAbi : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    // ------------------------------------------------------------------ library

    TEST_F(CAbi, GetVersion)
    {
        const char *version = DDBGetVersion();
        ASSERT_NE(version, nullptr);
        EXPECT_STRNE(version, "");
    }

    // ------------------------------------------------------------------ files and index

    TEST_F(CAbi, Info)
    {
        TestArea ta(TEST_NAME, true);
        const auto file = writeFile(ta.getPath("a.txt"), "alpha").string();
        const char *paths[] = {file.c_str()};
        const char *nullPath[] = {nullptr};
        char *out = nullptr;

        EXPECT_DDB_INVALID(DDBInfo(nullptr, 1, &out, "json"));
        EXPECT_DDB_INVALID(DDBInfo(paths, 0, &out, "json"));
        EXPECT_DDB_INVALID(DDBInfo(paths, -1, &out, "json"));
        EXPECT_DDB_INVALID(DDBInfo(nullPath, 1, &out, "json"));
        EXPECT_DDB_INVALID(DDBInfo(paths, 1, nullptr, "json"));
        EXPECT_DDB_INVALID(DDBInfo(paths, 1, &out, nullptr));
        EXPECT_DDB_INVALID(DDBInfo(paths, 1, &out, "json", false, 0, nullptr));
        EXPECT_DDB_INVALID(DDBInfo(paths, 1, &out, "json", false, -1));

        ASSERT_DDB_OK(DDBInfo(paths, 1, &out, "json"));
        const auto j = json::parse(take(out));
        ASSERT_TRUE(j.is_array());
        ASSERT_EQ(j.size(), 1u);
        EXPECT_EQ(j[0]["size"], 5);
    }

    TEST_F(CAbi, Remove)
    {
        Dataset ds(TEST_NAME);
        const auto file = (ds.root / "a.txt").string();
        const char *paths[] = {file.c_str()};
        const char *nullPath[] = {nullptr};

        EXPECT_DDB_INVALID(DDBRemove(nullptr, paths, 1));
        EXPECT_DDB_INVALID(DDBRemove("", paths, 1));
        EXPECT_DDB_INVALID(DDBRemove(ds.path().c_str(), nullptr, 1));
        EXPECT_DDB_INVALID(DDBRemove(ds.path().c_str(), paths, 0));
        EXPECT_DDB_INVALID(DDBRemove(ds.path().c_str(), nullPath, 1));

        ASSERT_DDB_OK(DDBRemove(ds.path().c_str(), paths, 1));
        char *out = nullptr;
        EXPECT_EQ(DDBGet(ds.path().c_str(), "a.txt", &out), DDBERR_NOTFOUND);
        DDBFree(out);
    }

    TEST_F(CAbi, Search)
    {
        Dataset ds(TEST_NAME);
        char *out = nullptr;

        EXPECT_DDB_INVALID(DDBSearch(nullptr, "*", &out, "json"));
        EXPECT_DDB_INVALID(DDBSearch(ds.path().c_str(), nullptr, &out, "json"));
        EXPECT_DDB_INVALID(DDBSearch(ds.path().c_str(), "*", nullptr, "json"));
        EXPECT_DDB_INVALID(DDBSearch(ds.path().c_str(), "*", &out, nullptr));
        EXPECT_DDB_INVALID(DDBSearch(ds.path().c_str(), "*", &out, ""));

        ASSERT_DDB_OK(DDBSearch(ds.path().c_str(), "*.txt", &out, "json"));
        const auto j = json::parse(take(out));
        ASSERT_EQ(j.size(), 1u);
        EXPECT_EQ(j[0]["path"], "a.txt");
    }

    TEST_F(CAbi, Status)
    {
        Dataset ds(TEST_NAME);
        char *out = nullptr;

        EXPECT_DDB_INVALID(DDBStatus(nullptr, &out));
        EXPECT_DDB_INVALID(DDBStatus(" ", &out));
        EXPECT_DDB_INVALID(DDBStatus(ds.path().c_str(), nullptr));

        ASSERT_DDB_OK(DDBStatus(ds.path().c_str(), &out));
        EXPECT_EQ(take(out), "");

        // Today the C API writes the status marker of each change, not its path
        writeFile(ds.root / "b.txt", "bravo");
        ASSERT_DDB_OK(DDBStatus(ds.path().c_str(), &out));
        EXPECT_EQ(take(out), "?\t");
    }

    TEST_F(CAbi, MoveEntry)
    {
        Dataset ds(TEST_NAME);

        EXPECT_DDB_INVALID(DDBMoveEntry(nullptr, "a.txt", "b.txt"));
        EXPECT_DDB_INVALID(DDBMoveEntry(ds.path().c_str(), nullptr, "b.txt"));
        EXPECT_DDB_INVALID(DDBMoveEntry(ds.path().c_str(), "a.txt", nullptr));
        EXPECT_DDB_INVALID(DDBMoveEntry(ds.path().c_str(), "", "b.txt"));
        EXPECT_DDB_INVALID(DDBMoveEntry(ds.path().c_str(), "missing.txt", "b.txt"));

        ASSERT_DDB_OK(DDBMoveEntry(ds.path().c_str(), "a.txt", "b.txt"));
        char *out = nullptr;
        ASSERT_DDB_OK(DDBGet(ds.path().c_str(), "b.txt", &out));
        EXPECT_EQ(json::parse(take(out))["path"], "b.txt");
        out = nullptr;
        EXPECT_EQ(DDBGet(ds.path().c_str(), "a.txt", &out), DDBERR_NOTFOUND);
        DDBFree(out);
        // The index entry moves; the file is left where it is (the caller moves it)
        EXPECT_TRUE(fs::exists(ds.root / "a.txt"));
        EXPECT_FALSE(fs::exists(ds.root / "b.txt"));
    }

    TEST_F(CAbi, Rescan)
    {
        Dataset ds(TEST_NAME);
        char *out = nullptr;

        EXPECT_DDB_INVALID(DDBRescan(nullptr, &out));
        EXPECT_DDB_INVALID(DDBRescan(ds.path().c_str(), nullptr));
        EXPECT_DDB_INVALID(DDBRescan(ds.path().c_str(), &out, "nosuchtype"));

        ASSERT_DDB_OK(DDBRescan(ds.path().c_str(), &out, nullptr));
        const auto j = json::parse(take(out));
        ASSERT_EQ(j.size(), 1u);
        EXPECT_EQ(j[0]["path"], "a.txt");
        EXPECT_EQ(j[0]["success"], true);
    }

    TEST_F(CAbi, Chattr)
    {
        Dataset ds(TEST_NAME);
        char *out = nullptr;

        EXPECT_DDB_INVALID(DDBChattr(nullptr, R"({"public":true})", &out));
        EXPECT_DDB_INVALID(DDBChattr(ds.path().c_str(), nullptr, &out));
        EXPECT_DDB_INVALID(DDBChattr(ds.path().c_str(), R"({"public":true})", nullptr));
        EXPECT_DDB_INVALID(DDBChattr(ds.path().c_str(), "not json", &out));
        EXPECT_DDB_INVALID(DDBChattr(ds.path().c_str(), R"({"nosuchattr":1})", &out));

        ASSERT_DDB_OK(DDBChattr(ds.path().c_str(), R"({"public":true})", &out));
        EXPECT_TRUE(json::parse(take(out)).is_object());
    }

    // ------------------------------------------------------------------ passwords

    TEST_F(CAbi, Passwords)
    {
        Dataset ds(TEST_NAME);
        bool verified = false;

        EXPECT_DDB_INVALID(DDBAppendPassword(nullptr, "secret"));
        EXPECT_DDB_INVALID(DDBAppendPassword(ds.path().c_str(), nullptr));
        EXPECT_DDB_INVALID(DDBAppendPassword(ds.path().c_str(), ""));
        EXPECT_DDB_INVALID(DDBVerifyPassword(nullptr, "secret", &verified));
        EXPECT_DDB_INVALID(DDBVerifyPassword(ds.path().c_str(), nullptr, &verified));
        EXPECT_DDB_INVALID(DDBVerifyPassword(ds.path().c_str(), "secret", nullptr));
        EXPECT_DDB_INVALID(DDBClearPasswords(nullptr));
        EXPECT_DDB_INVALID(DDBClearPasswords(""));

        // No password: the empty one verifies
        ASSERT_DDB_OK(DDBVerifyPassword(ds.path().c_str(), "", &verified));
        EXPECT_TRUE(verified);

        ASSERT_DDB_OK(DDBAppendPassword(ds.path().c_str(), "secret"));
        ASSERT_DDB_OK(DDBVerifyPassword(ds.path().c_str(), "secret", &verified));
        EXPECT_TRUE(verified);
        ASSERT_DDB_OK(DDBVerifyPassword(ds.path().c_str(), "wrong", &verified));
        EXPECT_FALSE(verified);
        ASSERT_DDB_OK(DDBVerifyPassword(ds.path().c_str(), "", &verified));
        EXPECT_FALSE(verified);

        ASSERT_DDB_OK(DDBClearPasswords(ds.path().c_str()));
        ASSERT_DDB_OK(DDBVerifyPassword(ds.path().c_str(), "", &verified));
        EXPECT_TRUE(verified);
    }

    // ------------------------------------------------------------------ tags

    TEST_F(CAbi, Tags)
    {
        Dataset ds(TEST_NAME);
        char *out = nullptr;

        EXPECT_DDB_INVALID(DDBSetTag(nullptr, "org/ds"));
        EXPECT_DDB_INVALID(DDBSetTag(ds.path().c_str(), nullptr));
        EXPECT_DDB_INVALID(DDBSetTag(ds.path().c_str(), "not a tag"));
        EXPECT_DDB_INVALID(DDBGetTag(nullptr, &out));
        EXPECT_DDB_INVALID(DDBGetTag(ds.path().c_str(), nullptr));

        ASSERT_DDB_OK(DDBGetTag(ds.path().c_str(), &out));
        EXPECT_EQ(take(out), "");

        ASSERT_DDB_OK(DDBSetTag(ds.path().c_str(), "org/ds"));
        ASSERT_DDB_OK(DDBGetTag(ds.path().c_str(), &out));
        EXPECT_NE(take(out).find("org/ds"), std::string::npos);
    }

    // ------------------------------------------------------------------ delta

    TEST_F(CAbi, DeltaAndLocals)
    {
        Dataset source(TEST_NAME + "-source");
        IndexFixture target(TEST_NAME + "-target");
        char *out = nullptr;

        ASSERT_DDB_OK(DDBGetStamp(source.path().c_str(), &out));
        const auto sourceStamp = take(out);
        ASSERT_DDB_OK(DDBGetStamp(target.root.string().c_str(), &out));
        const auto targetStamp = take(out);

        EXPECT_DDB_INVALID(DDBDelta(nullptr, targetStamp.c_str(), &out, "json"));
        EXPECT_DDB_INVALID(DDBDelta(sourceStamp.c_str(), nullptr, &out, "json"));
        EXPECT_DDB_INVALID(DDBDelta(sourceStamp.c_str(), targetStamp.c_str(), nullptr, "json"));
        EXPECT_DDB_INVALID(DDBDelta(sourceStamp.c_str(), targetStamp.c_str(), &out, nullptr));
        EXPECT_DDB_INVALID(DDBDelta(sourceStamp.c_str(), targetStamp.c_str(), &out, ""));
        EXPECT_DDB_INVALID(DDBDelta("not json", targetStamp.c_str(), &out, "json"));

        // What the target needs to become the source: add a.txt
        ASSERT_DDB_OK(DDBDelta(sourceStamp.c_str(), targetStamp.c_str(), &out, "json"));
        const auto delta = take(out);
        const auto d = json::parse(delta);
        ASSERT_EQ(d["adds"].size(), 1u);
        EXPECT_EQ(d["adds"][0]["path"], "a.txt");
        const std::string hash = d["adds"][0]["hash"];

        EXPECT_DDB_INVALID(DDBComputeDeltaLocals(nullptr, source.path().c_str(), "", &out));
        EXPECT_DDB_INVALID(DDBComputeDeltaLocals(delta.c_str(), nullptr, "", &out));
        EXPECT_DDB_INVALID(DDBComputeDeltaLocals(delta.c_str(), source.path().c_str(), nullptr, &out));
        EXPECT_DDB_INVALID(DDBComputeDeltaLocals(delta.c_str(), source.path().c_str(), "", nullptr));
        EXPECT_DDB_INVALID(DDBComputeDeltaLocals("not json", source.path().c_str(), "", &out));

        // The source holds the added file; the empty target does not
        ASSERT_DDB_OK(DDBComputeDeltaLocals(delta.c_str(), source.path().c_str(), "", &out));
        EXPECT_TRUE(json::parse(take(out)).contains(hash));
        ASSERT_DDB_OK(DDBComputeDeltaLocals(delta.c_str(), target.root.string().c_str(), "", &out));
        EXPECT_TRUE(json::parse(take(out)).empty());
    }

    // ------------------------------------------------------------------ build queries

    TEST_F(CAbi, BuildQueries)
    {
        Dataset ds(TEST_NAME);
        smallRaster(ds.root / "ortho.tif");
        addToIndex(ds.db.get(), {(ds.root / "ortho.tif").string()});
        bool flag = true;

        EXPECT_DDB_INVALID(DDBIsBuildable(nullptr, "a.txt", &flag));
        EXPECT_DDB_INVALID(DDBIsBuildable(ds.path().c_str(), nullptr, &flag));
        EXPECT_DDB_INVALID(DDBIsBuildable(ds.path().c_str(), "a.txt", nullptr));
        EXPECT_DDB_INVALID(DDBIsBuildPending(nullptr, &flag));
        EXPECT_DDB_INVALID(DDBIsBuildPending(ds.path().c_str(), nullptr));
        EXPECT_DDB_INVALID(DDBIsBuildActive(nullptr, "a.txt", &flag));
        EXPECT_DDB_INVALID(DDBIsBuildActive(ds.path().c_str(), nullptr, &flag));
        EXPECT_DDB_INVALID(DDBIsBuildActive(ds.path().c_str(), "", &flag));
        EXPECT_DDB_INVALID(DDBIsBuildActive(ds.path().c_str(), "a.txt", nullptr));

        ASSERT_DDB_OK(DDBIsBuildable(ds.path().c_str(), "a.txt", &flag));
        EXPECT_FALSE(flag);
        ASSERT_DDB_OK(DDBIsBuildable(ds.path().c_str(), "ortho.tif", &flag));
        EXPECT_TRUE(flag);

        flag = true;
        ASSERT_DDB_OK(DDBIsBuildPending(ds.path().c_str(), &flag));
        EXPECT_FALSE(flag);

        flag = true;
        ASSERT_DDB_OK(DDBIsBuildActive(ds.path().c_str(), "ortho.tif", &flag));
        EXPECT_FALSE(flag);
    }

    // ------------------------------------------------------------------ metadata

    TEST_F(CAbi, MetaInvalidArguments)
    {
        Dataset ds(TEST_NAME);
        const auto p = ds.path();
        char *out = nullptr;

        EXPECT_DDB_INVALID(DDBMetaAdd(nullptr, "", "annotations", "{}", &out));
        EXPECT_DDB_INVALID(DDBMetaAdd(p.c_str(), nullptr, "annotations", "{}", &out));
        EXPECT_DDB_INVALID(DDBMetaAdd(p.c_str(), "", nullptr, "{}", &out));
        EXPECT_DDB_INVALID(DDBMetaAdd(p.c_str(), "", "annotations", nullptr, &out));
        EXPECT_DDB_INVALID(DDBMetaAdd(p.c_str(), "", "annotations", "{}", nullptr));
        EXPECT_DDB_INVALID(DDBMetaAdd(p.c_str(), "", "no-such-key", "{}", &out));

        EXPECT_DDB_INVALID(DDBMetaSet(nullptr, "", "visibility", "1", &out));
        EXPECT_DDB_INVALID(DDBMetaSet(p.c_str(), nullptr, "visibility", "1", &out));
        EXPECT_DDB_INVALID(DDBMetaSet(p.c_str(), "", nullptr, "1", &out));
        EXPECT_DDB_INVALID(DDBMetaSet(p.c_str(), "", "visibility", nullptr, &out));
        EXPECT_DDB_INVALID(DDBMetaSet(p.c_str(), "", "visibility", "1", nullptr));

        EXPECT_DDB_INVALID(DDBMetaRemove(nullptr, "id", &out));
        EXPECT_DDB_INVALID(DDBMetaRemove(p.c_str(), nullptr, &out));
        EXPECT_DDB_INVALID(DDBMetaRemove(p.c_str(), "id", nullptr));

        EXPECT_DDB_INVALID(DDBMetaGet(nullptr, "", "visibility", &out));
        EXPECT_DDB_INVALID(DDBMetaGet(p.c_str(), nullptr, "visibility", &out));
        EXPECT_DDB_INVALID(DDBMetaGet(p.c_str(), "", nullptr, &out));
        EXPECT_DDB_INVALID(DDBMetaGet(p.c_str(), "", "visibility", nullptr));
        EXPECT_DDB_INVALID(DDBMetaGet(p.c_str(), "", "visibility", &out)); // not set

        EXPECT_DDB_INVALID(DDBMetaUnset(nullptr, "", "visibility", &out));
        EXPECT_DDB_INVALID(DDBMetaUnset(p.c_str(), nullptr, "visibility", &out));
        EXPECT_DDB_INVALID(DDBMetaUnset(p.c_str(), "", nullptr, &out));
        EXPECT_DDB_INVALID(DDBMetaUnset(p.c_str(), "", "visibility", nullptr));

        EXPECT_DDB_INVALID(DDBMetaList(nullptr, "", &out));
        EXPECT_DDB_INVALID(DDBMetaList(p.c_str(), nullptr, &out));
        EXPECT_DDB_INVALID(DDBMetaList(p.c_str(), "", nullptr));

        EXPECT_DDB_INVALID(DDBMetaDump(nullptr, "[]", &out));
        EXPECT_DDB_INVALID(DDBMetaDump(p.c_str(), nullptr, &out));
        EXPECT_DDB_INVALID(DDBMetaDump(p.c_str(), "[]", nullptr));
        EXPECT_DDB_INVALID(DDBMetaDump(p.c_str(), "not json", &out));

        EXPECT_DDB_INVALID(DDBMetaRestore(nullptr, "[]", &out));
        EXPECT_DDB_INVALID(DDBMetaRestore(p.c_str(), nullptr, &out));
        EXPECT_DDB_INVALID(DDBMetaRestore(p.c_str(), "[]", nullptr));
        EXPECT_DDB_INVALID(DDBMetaRestore(p.c_str(), "not json", &out));
    }

    TEST_F(CAbi, MetaLifecycle)
    {
        Dataset ds(TEST_NAME);
        const auto p = ds.path();
        char *out = nullptr;

        // Add (list key) and Set (single key) on an entry and on the dataset
        ASSERT_DDB_OK(DDBMetaAdd(p.c_str(), "a.txt", "annotations", R"({"note":"x"})", &out));
        const auto added = json::parse(take(out));
        const std::string id = added["id"];
        EXPECT_EQ(added["data"]["note"], "x");

        ASSERT_DDB_OK(DDBMetaSet(p.c_str(), "", "visibility", "1", &out));
        EXPECT_EQ(json::parse(take(out))["data"], 1);

        ASSERT_DDB_OK(DDBMetaGet(p.c_str(), "a.txt", "annotations", &out));
        const auto got = json::parse(take(out));
        ASSERT_TRUE(got.is_array());
        ASSERT_EQ(got.size(), 1u);
        EXPECT_EQ(got[0]["id"], id);

        ASSERT_DDB_OK(DDBMetaList(p.c_str(), "a.txt", &out));
        const auto list = json::parse(take(out));
        ASSERT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0]["key"], "annotations");
        EXPECT_EQ(list[0]["count"], 1);

        // Dump everything, remove, restore
        ASSERT_DDB_OK(DDBMetaDump(p.c_str(), "[]", &out));
        const auto dump = take(out);
        EXPECT_EQ(json::parse(dump).size(), 2u);

        ASSERT_DDB_OK(DDBMetaRemove(p.c_str(), id.c_str(), &out));
        EXPECT_EQ(json::parse(take(out))["removed"], 1);
        ASSERT_DDB_OK(DDBMetaUnset(p.c_str(), "", "visibility", &out));
        EXPECT_EQ(json::parse(take(out))["removed"], 1);

        ASSERT_DDB_OK(DDBMetaRestore(p.c_str(), dump.c_str(), &out));
        EXPECT_EQ(json::parse(take(out))["restored"], 2);
        ASSERT_DDB_OK(DDBMetaGet(p.c_str(), "a.txt", "annotations", &out));
        EXPECT_EQ(json::parse(take(out))[0]["id"], id);
    }

    // ------------------------------------------------------------------ STAC

    TEST_F(CAbi, Stac)
    {
        Dataset ds(TEST_NAME);
        smallRaster(ds.root / "ortho.tif");
        addToIndex(ds.db.get(), {(ds.root / "ortho.tif").string()});
        char *out = nullptr;

        EXPECT_DDB_INVALID(DDBStac(nullptr, nullptr, nullptr, nullptr, nullptr, &out));
        EXPECT_DDB_INVALID(DDBStac("", nullptr, nullptr, nullptr, nullptr, &out));
        EXPECT_DDB_INVALID(DDBStac(ds.path().c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));

        // The optional arguments may be null: the dataset's collection
        ASSERT_DDB_OK(DDBStac(ds.path().c_str(), nullptr, nullptr, nullptr, nullptr, &out));
        EXPECT_EQ(json::parse(take(out))["type"], "Collection");

        ASSERT_DDB_OK(DDBStac(ds.path().c_str(), "ortho.tif", "", "", "", &out));
        EXPECT_EQ(json::parse(take(out))["type"], "Feature");
    }

    // ------------------------------------------------------------------ thumbnails and tiles

    TEST_F(CAbi, Thumbnails)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = smallRaster(ta.getPath("ortho.tif")).string();
        const auto thumb = ta.getPath("thumb.webp").string();
        uint8_t *buf = nullptr;
        int size = 0;

        EXPECT_DDB_INVALID(DDBGenerateThumbnail(nullptr, 64, thumb.c_str()));
        EXPECT_DDB_INVALID(DDBGenerateThumbnail(raster.c_str(), 64, nullptr));
        EXPECT_DDB_INVALID(DDBGenerateThumbnail(raster.c_str(), -1, thumb.c_str()));
        EXPECT_DDB_INVALID(DDBGenerateThumbnail(ta.getPath("missing.tif").string().c_str(), 64, thumb.c_str()));

        EXPECT_DDB_INVALID(DDBGenerateMemoryThumbnail(nullptr, 64, &buf, &size));
        EXPECT_DDB_INVALID(DDBGenerateMemoryThumbnail(raster.c_str(), -1, &buf, &size));
        EXPECT_DDB_INVALID(DDBGenerateMemoryThumbnail(raster.c_str(), 64, nullptr, &size));
        EXPECT_DDB_INVALID(DDBGenerateMemoryThumbnail(raster.c_str(), 64, &buf, nullptr));
        EXPECT_DDB_INVALID(DDBGenerateMemoryThumbnail(raster.c_str(), 0, &buf, &size));

        EXPECT_DDB_INVALID(DDBGenerateMemoryThumbnailEx(nullptr, 64, nullptr, nullptr, nullptr, nullptr,
                                                        nullptr, nullptr, &buf, &size));
        EXPECT_DDB_INVALID(DDBGenerateMemoryThumbnailEx(raster.c_str(), -1, nullptr, nullptr, nullptr, nullptr,
                                                        nullptr, nullptr, &buf, &size));
        EXPECT_DDB_INVALID(DDBGenerateMemoryThumbnailEx(raster.c_str(), 64, nullptr, nullptr, nullptr, nullptr,
                                                        nullptr, nullptr, nullptr, &size));
        EXPECT_DDB_INVALID(DDBGenerateMemoryThumbnailEx(raster.c_str(), 64, nullptr, nullptr, nullptr, nullptr,
                                                        nullptr, nullptr, &buf, nullptr));

        ASSERT_DDB_OK(DDBGenerateThumbnail(raster.c_str(), 64, thumb.c_str()));
        EXPECT_GT(fs::file_size(thumb), 0u);

        ASSERT_DDB_OK(DDBGenerateMemoryThumbnail(raster.c_str(), 64, &buf, &size));
        const auto image = testutils::decodeImage(takeBuffer(buf, size));
        EXPECT_EQ(std::max(image.width, image.height), 64);

        // Every visualization parameter is optional (null)
        buf = nullptr;
        size = 0;
        ASSERT_DDB_OK(DDBGenerateMemoryThumbnailEx(raster.c_str(), 64, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                   nullptr, &buf, &size));
        EXPECT_FALSE(takeBuffer(buf, size).empty());
    }

    TEST_F(CAbi, Tiles)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = smallRaster(ta.getPath("ortho.tif"), 3, 512).string();
        GDALTiler tiler(raster, "", 256);
        const int z = tiler.getMinMaxZ().max;
        const auto bounds = tiler.getMinMaxCoordsForZ(z);
        const int x = bounds.min.x;
        const int y = bounds.min.y;
        char *tilePath = nullptr;
        uint8_t *buf = nullptr;
        int size = 0;

        EXPECT_DDB_INVALID(DDBTile(nullptr, z, x, y, &tilePath));
        EXPECT_DDB_INVALID(DDBTile(raster.c_str(), z, x, y, nullptr));
        EXPECT_DDB_INVALID(DDBTile(raster.c_str(), z, x, y, &tilePath, -1));
        EXPECT_DDB_INVALID(DDBTile(raster.c_str(), -1, x, y, &tilePath));
        EXPECT_DDB_INVALID(DDBTile(raster.c_str(), z, -1, y, &tilePath));

        EXPECT_DDB_INVALID(DDBMemoryTile(nullptr, z, x, y, &buf, &size));
        EXPECT_DDB_INVALID(DDBMemoryTile(raster.c_str(), z, x, y, nullptr, &size));
        EXPECT_DDB_INVALID(DDBMemoryTile(raster.c_str(), z, x, y, &buf, nullptr));
        EXPECT_DDB_INVALID(DDBMemoryTile(raster.c_str(), z, x, y, &buf, &size, -1));
        EXPECT_DDB_INVALID(DDBMemoryTile(raster.c_str(), z, x, -1, &buf, &size));
        EXPECT_DDB_INVALID(DDBMemoryTile(raster.c_str(), z, x, y, &buf, &size, 0)); // not a power of 2
        EXPECT_DDB_INVALID(DDBTile(ta.getPath("missing.tif").string().c_str(), z, x, y, &tilePath));
        DDBFree(tilePath); // DDBTile sets the output to "" before it fails
        tilePath = nullptr;

        ASSERT_DDB_OK(DDBTile(raster.c_str(), z, x, y, &tilePath));
        const auto path = take(tilePath);
        EXPECT_TRUE(fs::exists(path)) << path;

        // The hash is optional (null)
        ASSERT_DDB_OK(DDBMemoryTile(raster.c_str(), z, x, y, &buf, &size, 256, false, false, nullptr));
        const auto tile = testutils::decodeImage(takeBuffer(buf, size));
        EXPECT_EQ(tile.width, 256);
        EXPECT_EQ(tile.height, 256);
    }

    // ------------------------------------------------------------------ raster analysis

    TEST_F(CAbi, GetRasterMetadata)
    {
        TestArea ta(TEST_NAME, true);
        const auto raster = smallRaster(ta.getPath("ortho.tif")).string();
        char *out = nullptr;

        EXPECT_DDB_INVALID(DDBGetRasterMetadata(nullptr, nullptr, nullptr, &out));
        EXPECT_DDB_INVALID(DDBGetRasterMetadata(raster.c_str(), nullptr, nullptr, nullptr));
        EXPECT_DDB_INVALID(DDBGetRasterMetadata(ta.getPath("missing.tif").string().c_str(), nullptr, nullptr, &out));

        // formula and band filter are optional (null)
        ASSERT_DDB_OK(DDBGetRasterMetadata(raster.c_str(), nullptr, nullptr, &out));
        EXPECT_TRUE(json::parse(take(out)).is_object());
    }

    TEST_F(CAbi, MaskBorders)
    {
        TestArea ta(TEST_NAME, true);
        GeoRasterSpec spec;
        spec.width = 128;
        spec.height = 128;
        // A data square inside a black border
        spec.value = [](int, int x, int y) { return x >= 16 && x < 112 && y >= 16 && y < 112 ? 128.0 : 0.0; };
        const auto input = testutils::createGeoRaster(ta.getPath("ortho.tif"), spec).string();
        const auto output = ta.getPath("masked.tif").string();

        EXPECT_DDB_INVALID(DDBMaskBorders(nullptr, output.c_str(), 15, false));
        EXPECT_DDB_INVALID(DDBMaskBorders(input.c_str(), nullptr, 15, false));
        EXPECT_DDB_INVALID(DDBMaskBorders(input.c_str(), "", 15, false));
        EXPECT_DDB_INVALID(DDBMaskBorders(ta.getPath("missing.tif").string().c_str(), output.c_str(), 15, false));

        ASSERT_DDB_OK(DDBMaskBorders(input.c_str(), output.c_str(), 15, false));
        EXPECT_TRUE(fs::exists(output));
    }

    // ------------------------------------------------------------------ multispectral

    // Single-band rasters on the same grid, as a multispectral camera writes them
    std::vector<std::string> bandFiles(TestArea &ta, int count)
    {
        std::vector<std::string> files;
        for (int i = 0; i < count; i++)
            files.push_back(smallRaster(ta.getPath("band" + std::to_string(i + 1) + ".tif"), 1, 64).string());
        return files;
    }

    std::vector<const char *> cStrings(const std::vector<std::string> &strings)
    {
        std::vector<const char *> out;
        for (const auto &s : strings)
            out.push_back(s.c_str());
        return out;
    }

    TEST_F(CAbi, MergeMultispectral)
    {
        TestArea ta(TEST_NAME, true);
        const auto files = bandFiles(ta, 3);
        auto paths = cStrings(files);
        const auto merged = ta.getPath("merged.tif").string();
        char *out = nullptr;
        uint8_t *buf = nullptr;
        int size = 0;

        EXPECT_DDB_INVALID(DDBValidateMergeMultispectral(nullptr, 3, &out));
        EXPECT_DDB_INVALID(DDBValidateMergeMultispectral(paths.data(), 0, &out));
        EXPECT_DDB_INVALID(DDBValidateMergeMultispectral(paths.data(), 3, nullptr));

        EXPECT_DDB_INVALID(DDBPreviewMergeMultispectral(nullptr, 3, nullptr, 64, &buf, &size));
        EXPECT_DDB_INVALID(DDBPreviewMergeMultispectral(paths.data(), 0, nullptr, 64, &buf, &size));
        EXPECT_DDB_INVALID(DDBPreviewMergeMultispectral(paths.data(), 3, nullptr, 64, nullptr, &size));
        EXPECT_DDB_INVALID(DDBPreviewMergeMultispectral(paths.data(), 3, nullptr, 64, &buf, nullptr));
        EXPECT_DDB_INVALID(DDBPreviewMergeMultispectral(paths.data(), 3, "1,x,3", 64, &buf, &size));

        EXPECT_DDB_INVALID(DDBMergeMultispectral(nullptr, 3, merged.c_str()));
        EXPECT_DDB_INVALID(DDBMergeMultispectral(paths.data(), 1, merged.c_str()));
        EXPECT_DDB_INVALID(DDBMergeMultispectral(paths.data(), 3, nullptr));
        EXPECT_DDB_INVALID(DDBMergeMultispectral(paths.data(), 3, ""));

        // Null elements are skipped: nothing left to merge
        const char *nulls[] = {nullptr, nullptr, nullptr};
        EXPECT_DDB_INVALID(DDBPreviewMergeMultispectral(nulls, 3, nullptr, 64, &buf, &size));
        EXPECT_DDB_INVALID(DDBMergeMultispectral(nulls, 3, merged.c_str()));
        ASSERT_DDB_OK(DDBValidateMergeMultispectral(nulls, 3, &out));
        EXPECT_EQ(json::parse(take(out))["ok"], false);

        ASSERT_DDB_OK(DDBValidateMergeMultispectral(paths.data(), 3, &out));
        const auto validation = json::parse(take(out));
        EXPECT_EQ(validation["ok"], true) << validation.dump();
        EXPECT_EQ(validation["summary"]["totalBands"], 3);

        ASSERT_DDB_OK(DDBPreviewMergeMultispectral(paths.data(), 3, nullptr, 64, &buf, &size));
        EXPECT_FALSE(takeBuffer(buf, size).empty());

        ASSERT_DDB_OK(DDBMergeMultispectral(paths.data(), 3, merged.c_str()));
        GDALDatasetUniquePtr ds(GDALDataset::Open(merged.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
        ASSERT_TRUE(ds);
        EXPECT_EQ(ds->GetRasterCount(), 3);
    }

    // ------------------------------------------------------------------ alignment

    TEST_F(CAbi, AlignRaster)
    {
        TestArea ta(TEST_NAME, true);
        const auto reference = smallRaster(ta.getPath("reference.tif"), 3, 256).string();
        const auto source = smallRaster(ta.getPath("source.tif"), 3, 256).string();
        const auto aligned = ta.getPath("aligned.tif").string();
        char *out = nullptr;

        EXPECT_DDB_INVALID(DDBValidateAlignRaster(nullptr, reference.c_str(), &out));
        EXPECT_DDB_INVALID(DDBValidateAlignRaster(source.c_str(), nullptr, &out));
        EXPECT_DDB_INVALID(DDBValidateAlignRaster(source.c_str(), reference.c_str(), nullptr));

        EXPECT_DDB_INVALID(DDBAlignRaster(nullptr, reference.c_str(), aligned.c_str(), nullptr, &out));
        EXPECT_DDB_INVALID(DDBAlignRaster(source.c_str(), nullptr, aligned.c_str(), nullptr, &out));
        EXPECT_DDB_INVALID(DDBAlignRaster(source.c_str(), reference.c_str(), nullptr, nullptr, &out));
        EXPECT_DDB_INVALID(DDBAlignRaster(source.c_str(), reference.c_str(), aligned.c_str(), nullptr, nullptr));
        EXPECT_DDB_INVALID(DDBAlignRaster(source.c_str(), reference.c_str(), aligned.c_str(), "nosuchmode", &out));

        ASSERT_DDB_OK(DDBValidateAlignRaster(source.c_str(), reference.c_str(), &out));
        EXPECT_TRUE(json::parse(take(out)).is_object());

        ASSERT_DDB_OK(DDBAlignRaster(source.c_str(), reference.c_str(), aligned.c_str(), "translation", &out));
        EXPECT_TRUE(json::parse(take(out)).is_object());
        EXPECT_TRUE(fs::exists(aligned));
    }
}
