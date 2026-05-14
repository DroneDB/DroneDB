/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ddb.h"
#include "test.h"
#include "testarea.h"
#include "vector.h"
#include "fs.h"
#include <nlohmann/json.hpp>

namespace
{

    using json = nlohmann::json;

    // Builds a vector dataset and returns the path to the GPKG sidecar.
    static fs::path BuildAndGetGpkg(TestArea &ta, const std::string &assetUrl,
                                    const std::string &filename)
    {
        const fs::path src = ta.downloadTestAsset(assetUrl, filename);
        const fs::path baseDir = ta.getPath("build");
        ddb::buildVector(src.string(), baseDir.string());
        const fs::path gpkg = baseDir / "vec" / "source.gpkg";
        EXPECT_TRUE(fs::exists(gpkg)) << "Missing " << gpkg.string();
        return gpkg;
    }

    TEST(testVectorApi, DescribeGeoJson)
    {
        TestArea ta(TEST_NAME);
        const fs::path gpkg = BuildAndGetGpkg(
            ta,
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/test2.geojson",
            "test.geojson");

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
        TestArea ta(TEST_NAME);
        const fs::path gpkg = BuildAndGetGpkg(
            ta,
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/test2.geojson",
            "test.geojson");

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
        TestArea ta(TEST_NAME);
        const fs::path gpkg = BuildAndGetGpkg(
            ta,
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/test2.geojson",
            "test.geojson");

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
        TestArea ta(TEST_NAME);
        const fs::path gpkg = BuildAndGetGpkg(
            ta,
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/test2.geojson",
            "test.geojson");

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
        TestArea ta(TEST_NAME);
        const fs::path gpkg = BuildAndGetGpkg(
            ta,
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/vector/test2.geojson",
            "test.geojson");

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

    TEST(testVectorApi, FreeIsNoopOnNull)
    {
        // DDBFree accepts nullptr (matches free() semantics).
        EXPECT_EQ(DDBFree(nullptr), DDBERR_NONE);
        // DDBVSIFree rejects nullptr (documented as a precondition).
        EXPECT_NE(DDBVSIFree(nullptr), DDBERR_NONE);
    }

} // namespace
