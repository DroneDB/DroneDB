/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// getBuildStatus() / DDBGetBuildStatus answer isBuildable() and
// isBuildComplete() for a whole listing with one index open (the Registry used to
// make two native calls, i.e. two opens, per buildable entry).


#include "gtest/gtest.h"

#include "build.h"
#include "dbops.h"
#include "ddb.h"
#include "exceptions.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    const std::string H_ORTHO(64, 'a');
    const std::string H_DSM(64, 'b');
    const std::string H_CLOUD(64, 'c');
    const std::string H_ROADS(64, 'd');

    struct Fixture : IndexFixture
    {
        explicit Fixture(const std::string &name) : IndexFixture(name)
        {
            insert("ortho.tif", H_ORTHO, EntryType::GeoRaster);     // complete build
            insert("dsm.tif", H_DSM, EntryType::GeoRaster);         // never built
            insert("cloud.laz", H_CLOUD, EntryType::PointCloud);    // empty stub left by an aborted build
            insert("ept-data/x.laz", std::string(64, 'e'), EntryType::PointCloud); // not buildable
            insert("photo.jpg", std::string(64, 'f'), EntryType::GeoImage);
            insert("roads.shp", H_ROADS, EntryType::Vector);        // complete build
            insert("roads.dbf", std::string(64, '1'), EntryType::Generic); // dependency of roads.shp
            insert("orphan.dbf", std::string(64, '2'), EntryType::Generic); // dependency without main file
            insert("dir", "", EntryType::Directory);

            writeFile(db->buildDirectory() / H_ORTHO / "cog" / "cog.tif", "x");
            fs::create_directories(db->buildDirectory() / H_CLOUD / "copc");
            writeFile(db->buildDirectory() / H_ROADS / "vec" / "source.gpkg", "x");
            writeFile(db->buildDirectory() / H_ROADS / "mvt" / "metadata.json", "x");
        }

        void insert(const std::string &path, const std::string &hash, EntryType type) const
        {
            insertEntry(db.get(), path, hash, type);
        }
    };

    const std::vector<std::string> ALL_PATHS = {"ortho.tif", "dsm.tif", "cloud.laz", "ept-data/x.laz",
                                                "photo.jpg", "roads.shp", "roads.dbf", "orphan.dbf",
                                                "dir", "missing.tif"};

    TEST(DDB_BuildStatus, batchMatchesPerEntryCalls)
    {
        Fixture fx(TEST_NAME);

        const auto statuses = getBuildStatus(fx.db.get(), ALL_PATHS);
        ASSERT_EQ(statuses.size(), ALL_PATHS.size());

        for (size_t i = 0; i < ALL_PATHS.size(); i++)
        {
            const auto &path = ALL_PATHS[i];
            const auto &s = statuses[i];
            SCOPED_TRACE(path);
            EXPECT_EQ(s.path, path);

            Entry e;
            EXPECT_EQ(s.exists, loadEntry(fx.db.get(), path, e));

            const bool buildable = s.exists && isBuildable(fx.db.get(), path);
            EXPECT_EQ(s.buildable, buildable);
            EXPECT_EQ(s.complete, isBuildComplete(fx.db.get(), path));
        }
    }

    TEST(DDB_BuildStatus, expectedValues)
    {
        Fixture fx(TEST_NAME);
        const auto s = getBuildStatus(fx.db.get(), ALL_PATHS);

        auto at = [&](const std::string &p) -> const BuildStatus &
        {
            return s[std::find(ALL_PATHS.begin(), ALL_PATHS.end(), p) - ALL_PATHS.begin()];
        };

        EXPECT_TRUE(at("ortho.tif").buildable);
        EXPECT_TRUE(at("ortho.tif").complete);
        EXPECT_TRUE(at("dsm.tif").buildable);
        EXPECT_FALSE(at("dsm.tif").complete);
        EXPECT_TRUE(at("cloud.laz").buildable);
        EXPECT_FALSE(at("cloud.laz").complete) << "an empty output folder is not a complete build";
        EXPECT_FALSE(at("ept-data/x.laz").buildable);
        EXPECT_FALSE(at("photo.jpg").buildable);
        EXPECT_FALSE(at("photo.jpg").complete);
        EXPECT_TRUE(at("roads.shp").complete);
        EXPECT_TRUE(at("roads.dbf").buildable);
        EXPECT_TRUE(at("roads.dbf").complete) << "a dependency is complete when its main file is";
        // A sidecar without its main file is not buildable: "buildable
        // but never complete" made completeness sweeps re-enqueue it forever.
        EXPECT_FALSE(at("orphan.dbf").buildable);
        EXPECT_FALSE(at("orphan.dbf").complete);
        EXPECT_FALSE(at("dir").buildable);
        EXPECT_FALSE(at("missing.tif").exists);
        EXPECT_FALSE(at("missing.tif").buildable);
        EXPECT_FALSE(at("missing.tif").complete);
    }

    TEST(DDB_BuildStatus, cApiKeepsInputOrderAndDuplicates)
    {
        Fixture fx(TEST_NAME);
        fx.db->close();

        const char *paths[] = {"dsm.tif", "ortho.tif", "missing.tif", "ortho.tif"};
        char *out = nullptr;
        ASSERT_EQ(DDBGetBuildStatus(fx.root.string().c_str(), paths, 4, &out), DDBERR_NONE) << DDBGetLastError();
        const auto j = json::parse(out);
        DDBFree(out);

        ASSERT_EQ(j.size(), 4u);
        EXPECT_EQ(j[0]["path"], "dsm.tif");
        EXPECT_EQ(j[0]["buildable"], true);
        EXPECT_EQ(j[0]["complete"], false);
        EXPECT_EQ(j[1]["path"], "ortho.tif");
        EXPECT_EQ(j[1]["complete"], true);
        EXPECT_EQ(j[2]["exists"], false);
        EXPECT_EQ(j[3], j[1]);
    }

    TEST(DDB_BuildStatus, cApiEmptyListAndBadArguments)
    {
        Fixture fx(TEST_NAME);
        fx.db->close();

        const char *paths[] = {"dsm.tif"};
        char *out = nullptr;
        ASSERT_EQ(DDBGetBuildStatus(fx.root.string().c_str(), paths, 0, &out), DDBERR_NONE) << DDBGetLastError();
        EXPECT_STREQ(out, "[]");
        DDBFree(out);

        EXPECT_NE(DDBGetBuildStatus(fx.root.string().c_str(), paths, 1, nullptr), DDBERR_NONE);
        EXPECT_NE(DDBGetBuildStatus(fx.root.string().c_str(), nullptr, 1, &out), DDBERR_NONE);
        EXPECT_NE(DDBGetBuildStatus("", paths, 1, &out), DDBERR_NONE);
    }
} // namespace
