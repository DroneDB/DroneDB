/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The JSON of the index types is written by ADL to_json / from_json. The shapes are
// frozen by tests/contract (the fixtures run in ContractFixtures); these pin the members.

#include "gtest/gtest.h"

#include "build_manifest.h"
#include "dbops.h"
#include "entry.h"
#include "entry_path.h"
#include "gsplat.h"
#include "pointcloud.h"

namespace
{
    using namespace ddb;

    Entry sampleEntry()
    {
        Entry e;
        e.path = "dir/a.jpg";
        e.hash = "abc";
        e.type = EntryType::GeoImage;
        e.properties = json{{"width", 10}};
        e.mtime = 1700000000;
        e.size = 42;
        e.depth = 1;
        e.point_geom.addPoint(9.1, 45.2, 100.0);
        e.meta = json{{"name", json{{"id", "x"}}}};
        return e;
    }

    TEST(EntryJson, AnEntryWritesEveryMemberThatIsSet)
    {
        const json j = sampleEntry();
        EXPECT_EQ(j.at("path"), "dir/a.jpg");
        EXPECT_EQ(j.at("hash"), "abc");
        EXPECT_EQ(j.at("type"), 3);
        EXPECT_EQ(j.at("properties"), (json{{"width", 10}}));
        EXPECT_EQ(j.at("mtime"), 1700000000);
        EXPECT_EQ(j.at("size"), 42);
        EXPECT_EQ(j.at("depth"), 1);
        EXPECT_TRUE(j.contains("point_geom"));
        EXPECT_FALSE(j.contains("polygon_geom"));
        EXPECT_EQ(j.at("meta"), (json{{"name", json{{"id", "x"}}}}));
    }

    TEST(EntryJson, AnEmptyEntryWritesOnlyThePathTypeTimeSizeAndDepth)
    {
        Entry e;
        e.path = "a";
        const json j = e;
        EXPECT_EQ(j, (json{{"path", "a"}, {"type", 0}, {"mtime", 0}, {"size", 0}, {"depth", 0}}));
    }

    TEST(EntryJson, AnEntryReadsBackWhatFromJsonReads)
    {
        const Entry source = sampleEntry();
        const auto read = json(source).get<Entry>();
        EXPECT_EQ(read.path, source.path);
        EXPECT_EQ(read.hash, source.hash);
        EXPECT_EQ(read.type, source.type);
        EXPECT_EQ(read.size, source.size);
        EXPECT_EQ(read.depth, source.depth);
        EXPECT_EQ(read.mtime, source.mtime);

        EXPECT_THROW((json{{"path", "a"}}.get<Entry>()), json::exception) << "type, size, depth and mtime are required";
        const auto withNullHash = json{{"path", "a"}, {"hash", nullptr}, {"type", 2}, {"size", 1}, {"depth", 0}, {"mtime", 5}}.get<Entry>();
        EXPECT_EQ(withNullHash.hash, "");
    }

    TEST(EntryJson, AnEntryGeoJsonIsTheFirstGeometryWithTheEntryAsProperties)
    {
        const Entry e = sampleEntry();
        const auto feature = e.toGeoJSON();
        ASSERT_TRUE(feature);
        EXPECT_EQ(feature->at("type"), "Feature");
        EXPECT_EQ(feature->at("geometry").at("type"), "Point");
        EXPECT_EQ(feature->at("properties").at("path"), "dir/a.jpg");
        EXPECT_EQ(feature->at("properties").at("width"), 10);
        EXPECT_TRUE(feature->at("properties").contains("meta"));

        EXPECT_FALSE(e.toGeoJSON(BasicGeometryType::BGPolygon)) << "the entry has no polygon";
        EXPECT_FALSE(Entry().toGeoJSON());
    }

    TEST(EntryJson, AnAddResultListsEntriesUnchangedAndErrors)
    {
        AddResult result;
        result.entries.emplace_back(sampleEntry(), false);
        Entry updated = sampleEntry();
        updated.path = "b";
        result.entries.emplace_back(updated, true);
        result.unchanged.push_back("c");
        result.errors.push_back({"d", "FS", "gone"});

        const json j = result;
        ASSERT_EQ(j.at("entries").size(), 2u);
        EXPECT_EQ(j.at("entries")[0].at("status"), "added");
        EXPECT_EQ(j.at("entries")[1].at("status"), "updated");
        EXPECT_EQ(j.at("entries")[0].at("path"), "dir/a.jpg");
        EXPECT_EQ(j.at("unchanged"), (json::array({json{{"path", "c"}}})));
        EXPECT_EQ(j.at("errors"), (json::array({json{{"path", "d"}, {"code", "FS"}, {"message", "gone"}}})));
    }

    TEST(EntryJson, AnEmptyAddResultHasThreeEmptyArrays)
    {
        EXPECT_EQ(json(AddResult{}), (json{{"entries", json::array()}, {"unchanged", json::array()}, {"errors", json::array()}}));
    }

    TEST(EntryJson, TheBuildTypesWriteTheirManifestMembers)
    {
        BuildArtifact artifact;
        artifact.role = "tiles";
        artifact.path = "ept";
        artifact.kind = ArtifactKind::Directory;
        artifact.mediaType = "application/json";
        artifact.index = "ept.json";
        const json a = artifact;
        EXPECT_EQ(a.at("kind"), "dir");
        EXPECT_EQ(a.at("index"), "ept.json");
        EXPECT_FALSE(a.contains("downloadName"));
        EXPECT_FALSE(a.contains("legacy"));

        EntryBuild build;
        build.artifacts.push_back(artifact);
        const json b = build;
        EXPECT_EQ(b.at("artifacts").size(), 1u);
        EXPECT_TRUE(b.at("source").is_null());
        EXPECT_TRUE(b.at("root").is_null());
        EXPECT_TRUE(b.at("lastAttempt").is_null());
        EXPECT_EQ(b.at("buildable"), false);
    }

    TEST(EntryJson, TheEntryPathResultIsThePathOrTheRejection)
    {
        EXPECT_EQ(json(normalizeEntryPath("a//b/./c", {})), (json{{"path", "a/b/c"}}));
        EXPECT_EQ(json(normalizeEntryPath("../x", {})), (json{{"rejected", "traversal"}}));
    }

    TEST(EntryJson, TheEntryPropertiesOfAPointCloudAndASplat)
    {
        PointCloudInfo cloud;
        cloud.pointCount = 7;
        cloud.wktProjection = "WKT";
        cloud.dimensions = {"X", "Y"};
        EXPECT_EQ(json(cloud), (json{{"pointCount", 7}, {"projection", "WKT"}, {"dimensions", json::array({"X", "Y"})}}));

        GaussianSplatInfo splat;
        splat.splatCount = 5;
        splat.shDegree = 2;
        EXPECT_EQ(json(splat), (json{{"splatCount", 5}, {"shDegree", 2}}));

        GsplatGeoref georef;
        georef.srs = "EPSG:32632";
        georef.offset = {1.0, 2.0, 3.0};
        EXPECT_EQ(json(georef), (json{{"srs", "EPSG:32632"}, {"offset", json::array({1.0, 2.0, 3.0})}}));
        georef.boundsWgs84 = {1, 2, 3, 4};
        EXPECT_EQ(json(georef).at("boundsWgs84"), (json::array({1, 2, 3, 4})));
    }
}
