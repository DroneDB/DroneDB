/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build_layout.h"
#include "database.h"
#include "ddb.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"
#include "vector.h"
#include "vector_query.h"
#include "raster_cache.h"
#include "fs.h"
#include "gdal_inc.h"
#include "ogrsf_frmts.h"
#include <nlohmann/json.hpp>
#include <array>
#include <functional>
#include <regex>
#include <set>

namespace
{

    using json = nlohmann::json;

    // Builds a vector dataset and returns the path to the GPKG sidecar. The queries read the
    // GeoPackage of a build, which lies in the build folder of its dataset: only there is a file
    // one that libddb wrote.
    static fs::path BuildAndGetGpkg(TestArea &ta, const std::string &assetUrl,
                                    const std::string &filename)
    {
        const fs::path src = ta.downloadTestAsset(assetUrl, filename);
        const fs::path baseDir = ta.getPath("dataset") / DDB_FOLDER / DDB_BUILD_PATH /
                                 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        ddb::buildVector(src.string(), baseDir.string());
        const fs::path gpkg = baseDir / "vec" / "source.gpkg";
        EXPECT_TRUE(fs::exists(gpkg)) << "Missing " << gpkg.string();
        return gpkg;
    }

    // Shared GPKG for tests that use test2.geojson: built once per binary run,
    // then reused (buildVector is idempotent and the TestArea persists across
    // test executions in the same temp directory).
    static const fs::path& sharedTest2GeoJsonGpkg()
    {
        static TestArea ta("testVectorApi_shared");
        static const fs::path gpkg = BuildAndGetGpkg(
            ta,
            testDataUrl("vector/test2.geojson"),
            "test.geojson");
        return gpkg;
    }

    TEST(testVectorApi, DescribeGeoJson)
    {
        const fs::path& gpkg = sharedTest2GeoJsonGpkg();

        char *out = nullptr;
        const DDBErr err = DDBDescribeVector(gpkg.string().c_str(), nullptr, &out);
        EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        ASSERT_NE(out, nullptr);

        json j = json::parse(out);
        EXPECT_TRUE(j.contains("driver"));
        ASSERT_TRUE(j.contains("layers"));
        ASSERT_TRUE(j["layers"].is_array());
        ASSERT_FALSE(j["layers"].empty());

        const auto &layer = j["layers"][0];
        EXPECT_TRUE(layer.contains("name"));
        EXPECT_TRUE(layer.contains("geometryType"));
        EXPECT_TRUE(layer.contains("srs"));
        EXPECT_TRUE(layer.contains("extent"));
        EXPECT_TRUE(layer.contains("fields"));
        EXPECT_TRUE(layer["fields"].is_array());

        DDBFree(out);
    }

    TEST(testVectorApi, QueryGeoJson)
    {
        const fs::path& gpkg = sharedTest2GeoJsonGpkg();

        char *out = nullptr;
        const DDBErr err = DDBQueryVector(
            gpkg.string().c_str(),
            /*layerName*/ nullptr,
            /*bbox*/ nullptr, /*bboxSrs*/ nullptr,
            /*maxFeatures*/ 100, /*startIndex*/ 0,
            /*outputFormat*/ "application/json",
            &out);
        EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        ASSERT_NE(out, nullptr);

        json j = json::parse(out);
        EXPECT_EQ(j.value("type", ""), "FeatureCollection");
        ASSERT_TRUE(j.contains("features"));
        EXPECT_TRUE(j["features"].is_array());

        DDBFree(out);
    }

    TEST(testVectorApi, QueryWithBbox)
    {
        const fs::path& gpkg = sharedTest2GeoJsonGpkg();

        // test2.geojson features lie around lon[4.55, 7.07] lat[51.72, 52.61]
        const double bboxOutside[4] = {-180.0, -90.0, -179.0, -89.0};
        const double bboxInside[4]  = {4.0,    51.0,   8.0,    53.0};

        char *outOutside = nullptr;
        DDBErr err = DDBQueryVector(gpkg.string().c_str(), nullptr,
                                    bboxOutside, "EPSG:4326",
                                    100, 0, "application/json", &outOutside);
        EXPECT_EQ(err, DDBERR_NONE);
        ASSERT_NE(outOutside, nullptr);
        json jOut = json::parse(outOutside);
        const size_t outsideCount = jOut.at("features").size();
        DDBFree(outOutside);

        char *outInside = nullptr;
        err = DDBQueryVector(gpkg.string().c_str(), nullptr,
                             bboxInside, "EPSG:4326",
                             100, 0, "application/json", &outInside);
        EXPECT_EQ(err, DDBERR_NONE);
        ASSERT_NE(outInside, nullptr);
        json jIn = json::parse(outInside);
        const size_t insideCount = jIn.at("features").size();
        DDBFree(outInside);

        EXPECT_GT(insideCount, outsideCount);
        EXPECT_EQ(outsideCount, 0u);
    }

    TEST(testVectorApi, QueryLimitClampMax)
    {
        const fs::path& gpkg = sharedTest2GeoJsonGpkg();

        // maxFeatures=1 should cap results
        char *out = nullptr;
        const DDBErr err = DDBQueryVector(gpkg.string().c_str(), nullptr,
                                          nullptr, nullptr,
                                          1, 0, "application/json", &out);
        EXPECT_EQ(err, DDBERR_NONE);
        ASSERT_NE(out, nullptr);
        json j = json::parse(out);
        EXPECT_LE(j.at("features").size(), 1u);
        DDBFree(out);
    }

    TEST(testVectorApi, QueryGml)
    {
        const fs::path& gpkg = sharedTest2GeoJsonGpkg();

        char *out = nullptr;
        const DDBErr err = DDBQueryVector(gpkg.string().c_str(), nullptr,
                                          nullptr, nullptr,
                                          50, 0, "application/gml+xml", &out);
        EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        ASSERT_NE(out, nullptr);
        const std::string gml(out);
        EXPECT_NE(gml.find("<?xml"), std::string::npos);
        DDBFree(out);
    }

    TEST(testVectorApi, InvalidInputs)
    {
        char *out = nullptr;

        // Null path
        DDBErr err = DDBDescribeVector(nullptr, nullptr, &out);
        EXPECT_NE(err, DDBERR_NONE);

        err = DDBQueryVector(nullptr, nullptr, nullptr, nullptr,
                             10, 0, "application/json", &out);
        EXPECT_NE(err, DDBERR_NONE);

        // Nonexistent file
        err = DDBQueryVector("does-not-exist.gpkg", nullptr,
                             nullptr, nullptr,
                             10, 0, "application/json", &out);
        EXPECT_NE(err, DDBERR_NONE);

        err = DDBDescribeVector("does-not-exist.gpkg", nullptr, &out);
        EXPECT_NE(err, DDBERR_NONE);
    }

    // ---- Stable feature ids, fid lookups, counts, bbox with paging ---------------------------

    // Feature ids in a GML response, in order
    std::vector<std::string> gmlIds(const std::string &gml)
    {
        std::vector<std::string> ids;
        static const std::regex featureId(R"re(<ogr:\w+ gml:id="([^"]+)")re");
        for (std::sregex_iterator it(gml.begin(), gml.end(), featureId), end; it != end; ++it)
            ids.push_back((*it)[1]);
        return ids;
    }

    std::string query(const fs::path &gpkg, const ddb::VectorQuery &q)
    {
        return ddb::queryVector(gpkg.string(), q);
    }

    ddb::VectorQuery gmlQuery(int maxFeatures, int startIndex)
    {
        ddb::VectorQuery q;
        q.maxFeatures = maxFeatures;
        q.startIndex = startIndex;
        q.outputFormat = "gml";
        return q;
    }

    // The bbox of one feature, in EPSG:4326
    std::array<double, 4> featureBbox(const fs::path &gpkg, int64_t fid)
    {
        ddb::VectorQuery q;
        q.fids = {fid};
        const auto fc = json::parse(query(gpkg, q));
        double minX = 180, minY = 90, maxX = -180, maxY = -90;
        std::function<void(const json &)> visit = [&](const json &c)
        {
            if (c.is_array() && !c.empty() && c[0].is_number())
            {
                minX = std::min(minX, c[0].get<double>());
                maxX = std::max(maxX, c[0].get<double>());
                minY = std::min(minY, c[1].get<double>());
                maxY = std::max(maxY, c[1].get<double>());
            }
            else if (c.is_array())
                for (const auto &child : c)
                    visit(child);
        };
        visit(fc["features"][0]["geometry"]["coordinates"]);
        return {minX, minY, maxX, maxY};
    }

    TEST(VectorQuery, featureIdsAreTheFidsInEveryPage)
    {
        const fs::path &gpkg = sharedTest2GeoJsonGpkg();

        const auto all = gmlIds(query(gpkg, gmlQuery(10, 0)));
        ASSERT_EQ(all.size(), 4u);
        EXPECT_EQ(all, (std::vector<std::string>{"test.1", "test.2", "test.3", "test.4"}));

        // Pages (the second one goes through SQL) keep the ids and the layer name
        EXPECT_EQ(gmlIds(query(gpkg, gmlQuery(2, 0))), (std::vector<std::string>{"test.1", "test.2"}));
        EXPECT_EQ(gmlIds(query(gpkg, gmlQuery(2, 2))), (std::vector<std::string>{"test.3", "test.4"}));

        // GeoJSON features carry the fid as id
        ddb::VectorQuery q;
        q.startIndex = 3;
        const auto fc = json::parse(query(gpkg, q));
        ASSERT_EQ(fc["features"].size(), 1u);
        EXPECT_EQ(fc["features"][0]["id"], 4);
    }

    TEST(VectorQuery, fidsSelectFeaturesByPrimaryKey)
    {
        const fs::path &gpkg = sharedTest2GeoJsonGpkg();

        auto q = gmlQuery(10, 0);
        q.fids = {4, 2, 99};
        EXPECT_EQ(gmlIds(query(gpkg, q)), (std::vector<std::string>{"test.2", "test.4"}));

        q.startIndex = 1;
        EXPECT_EQ(gmlIds(query(gpkg, q)), (std::vector<std::string>{"test.4"}));

        q.fids = {99};
        q.startIndex = 0;
        EXPECT_TRUE(gmlIds(query(gpkg, q)).empty());

        // C API
        const long long fids[] = {3};
        char *out = nullptr;
        ASSERT_EQ(DDBQueryVectorEx(gpkg.string().c_str(), nullptr, nullptr, nullptr, fids, 1, nullptr, 10, 0,
                                   "application/gml+xml", &out),
                  DDBERR_NONE)
            << DDBGetLastError();
        EXPECT_EQ(gmlIds(out), (std::vector<std::string>{"test.3"}));
        DDBFree(out);
        EXPECT_NE(DDBQueryVectorEx(gpkg.string().c_str(), nullptr, nullptr, nullptr, nullptr, 2, nullptr, 10, 0,
                                   "application/json", &out),
                  DDBERR_NONE);
    }

    TEST(VectorQuery, countsMatchTheQueries)
    {
        const fs::path &gpkg = sharedTest2GeoJsonGpkg();

        ddb::VectorQuery q;
        EXPECT_EQ(ddb::countVectorFeatures(gpkg.string(), q), 4);
        q.fids = {1, 3, 99};
        EXPECT_EQ(ddb::countVectorFeatures(gpkg.string(), q), 2);

        q = {};
        q.bbox = featureBbox(gpkg, 2);
        const auto inBbox = gmlIds(query(gpkg, [&] { auto g = gmlQuery(10, 0); g.bbox = q.bbox; return g; }()));
        ASSERT_FALSE(inBbox.empty());
        EXPECT_NE(std::find(inBbox.begin(), inBbox.end(), "test.2"), inBbox.end());
        EXPECT_EQ(ddb::countVectorFeatures(gpkg.string(), q), static_cast<int64_t>(inBbox.size()));

        long long count = -1;
        const double bbox[] = {(*q.bbox)[0], (*q.bbox)[1], (*q.bbox)[2], (*q.bbox)[3]};
        ASSERT_EQ(DDBCountVector(gpkg.string().c_str(), nullptr, bbox, "EPSG:4326", nullptr, 0, nullptr, &count), DDBERR_NONE);
        EXPECT_EQ(count, static_cast<long long>(inBbox.size()));
    }

    // WFS filters translated to SQL: the where expression filters queries (both paths) and
    // counts. Literals are quoted text: the column type decides the comparison (SQLite
    // affinity), so "id" > '99999' compares numbers.
    TEST(VectorQuery, whereFiltersQueriesAndCounts)
    {
        const fs::path &gpkg = sharedTest2GeoJsonGpkg();
        const auto ids = [&](const std::string &where, int maxFeatures = 10, int startIndex = 0) {
            auto q = gmlQuery(maxFeatures, startIndex);
            q.where = where;
            return gmlIds(query(gpkg, q));
        };
        const auto count = [&](const std::string &where, std::vector<int64_t> fids = {}) {
            ddb::VectorQuery q;
            q.where = where;
            q.fids = std::move(fids);
            return ddb::countVectorFeatures(gpkg.string(), q);
        };

        using Ids = std::vector<std::string>;
        EXPECT_EQ(ids("\"id\" > '162900'"), (Ids{"test.1", "test.2"}));
        EXPECT_EQ(ids("\"id\" > '99999'"), (Ids{"test.1", "test.2", "test.3", "test.4"})) << "numeric column";
        EXPECT_EQ(ids("\"name\" LIKE 'stadsregio%'"), (Ids{"test.3", "test.4"}));
        EXPECT_EQ(ids("\"name\" GLOB 'Regio*'"), (Ids{"test.2"}));
        EXPECT_EQ(ids("\"name\" GLOB 'regio*'"), Ids{}) << "GLOB is case-sensitive";
        EXPECT_EQ(ids("\"id\" > '162900'", 10, 1), (Ids{"test.2"})) << "the paged path (SQL) applies it too";
        EXPECT_EQ(ids("\"id\" > '162900' OR \"name\" = 'Regio Twente'", 1, 0), (Ids{"test.1"}));

        EXPECT_EQ(count("\"id\" > '162900'"), 2);
        EXPECT_EQ(count("\"id\" > '162900'", {2, 3}), 1) << "with feature ids";
        EXPECT_EQ(count("\"name\" IS NULL"), 0);

        // C API
        char *out = nullptr;
        ASSERT_EQ(DDBQueryVectorEx(gpkg.string().c_str(), nullptr, nullptr, nullptr, nullptr, 0,
                                   "\"name\" = 'Regio Twente'", 10, 0, "application/gml+xml", &out),
                  DDBERR_NONE)
            << DDBGetLastError();
        EXPECT_EQ(gmlIds(out), (Ids{"test.2"}));
        DDBFree(out);
        long long n = -1;
        ASSERT_EQ(DDBCountVector(gpkg.string().c_str(), nullptr, nullptr, nullptr, nullptr, 0,
                                 "\"id\" < '162900'", &n),
                  DDBERR_NONE);
        EXPECT_EQ(n, 2);

        // Malformed SQL is an error, not an empty result
        EXPECT_ANY_THROW(ids("\"id\" >"));
        EXPECT_ANY_THROW(ids("\"id\" >", 10, 1));
        EXPECT_NE(DDBCountVector(gpkg.string().c_str(), nullptr, nullptr, nullptr, nullptr, 0, "\"id\" >", &n),
                  DDBERR_NONE);
    }

    // The bbox used to be dropped for pages after the first (startIndex > 0 goes through SQL)
    TEST(VectorQuery, bboxAppliesToLaterPages)
    {
        const fs::path &gpkg = sharedTest2GeoJsonGpkg();

        auto first = gmlQuery(1, 0);
        first.bbox = featureBbox(gpkg, 2);
        const auto all = gmlIds(query(gpkg, [&] { auto g = first; g.maxFeatures = 10; return g; }()));
        ASSERT_LT(all.size(), 4u) << "the bbox must select a subset";

        std::vector<std::string> paged;
        for (int start = 0; start < 6; start++)
        {
            auto page = first;
            page.startIndex = start;
            for (const auto &id : gmlIds(query(gpkg, page)))
                paged.push_back(id);
        }
        EXPECT_EQ(paged, all);
    }

    // Queries reuse an open dataset, and the filters of one never leak into the next
    TEST(VectorQuery, reusedDatasetsStartWithoutFilters)
    {
        const fs::path &gpkg = sharedTest2GeoJsonGpkg();
        auto &cache = ddb::RasterCache::instance();
        cache.release(gpkg);
        const auto before = cache.stats();

        ddb::VectorQuery filtered;
        filtered.bbox = featureBbox(gpkg, 2);
        filtered.fids = {2};
        EXPECT_EQ(ddb::countVectorFeatures(gpkg.string(), filtered), 1);
        EXPECT_EQ(gmlIds(query(gpkg, [&] { auto g = gmlQuery(10, 0); g.fids = {2}; g.bbox = filtered.bbox; return g; }())),
                  (std::vector<std::string>{"test.2"}));

        EXPECT_EQ(ddb::countVectorFeatures(gpkg.string(), {}), 4);
        EXPECT_EQ(gmlIds(query(gpkg, gmlQuery(10, 0))).size(), 4u);

        const auto after = cache.stats();
        EXPECT_GE(after.reused - before.reused, 3u) << "one open, then idle reuse";
    }

    TEST(testVectorApi, FreeIsNoopOnNull)
    {
        // Both DDBFree and DDBVSIFree accept nullptr (matches C free() semantics).
        EXPECT_EQ(DDBFree(nullptr), DDBERR_NONE);
        EXPECT_EQ(DDBVSIFree(nullptr), DDBERR_NONE);
    }

} // namespace
