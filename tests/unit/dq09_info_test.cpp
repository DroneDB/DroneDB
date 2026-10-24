/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-09: `ddb info` output formats.

#include <sstream>

#include "gtest/gtest.h"

#include "exceptions.h"
#include "info.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    const char *kPointGeoJson =
        R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"Point","coordinates":[9.1,45.2]}}]})";

    // `ddb info` on files of which only some have a footprint: the geojson output is a feature
    // collection of those. A file without geometry that came first used to leave a comma behind,
    // "features":[,{...}], which no JSON parser reads.
    TEST(Dq09Info, GeoJsonOfAFileWithoutGeometryBeforeOneWithGeometryIsValidJson)
    {
        TestArea ta(TEST_NAME);
        const auto text = writeFile(ta.getFolder() / "a.txt", "x");
        const auto point = writeFile(ta.getFolder() / "b.geojson", kPointGeoJson);

        std::ostringstream out;
        info({text.string(), point.string()}, out, {.format = OutputFormat::GeoJson});

        const json collection = json::parse(out.str());
        EXPECT_EQ(collection.at("type"), "FeatureCollection");
        ASSERT_EQ(collection.at("features").size(), 1u);
        EXPECT_EQ(collection.at("features")[0].at("geometry").at("type"), "Point");
    }

    TEST(Dq09Info, GeoJsonOfFilesWithoutGeometryIsAnEmptyCollection)
    {
        TestArea ta(TEST_NAME);
        const auto text = writeFile(ta.getFolder() / "a.txt", "x");

        std::ostringstream out;
        info({text.string()}, out, {.format = OutputFormat::GeoJson});
        EXPECT_EQ(json::parse(out.str()).at("features"), json::array());
    }

    TEST(Dq09Info, GeoJsonSeparatesTheFeaturesThatWereWritten)
    {
        TestArea ta(TEST_NAME);
        const auto first = writeFile(ta.getFolder() / "a.geojson", kPointGeoJson);
        const auto text = writeFile(ta.getFolder() / "b.txt", "x");
        const auto second = writeFile(ta.getFolder() / "c.geojson", kPointGeoJson);

        std::ostringstream out;
        info({first.string(), text.string(), second.string()}, out, {.format = OutputFormat::GeoJson});
        EXPECT_EQ(json::parse(out.str()).at("features").size(), 2u);
    }

    TEST(Dq09Info, JsonIsAnArrayOfEveryEntry)
    {
        TestArea ta(TEST_NAME);
        const auto text = writeFile(ta.getFolder() / "a.txt", "x");
        const auto point = writeFile(ta.getFolder() / "b.geojson", kPointGeoJson);

        std::ostringstream out;
        info({text.string(), point.string()}, out, {.format = OutputFormat::Json});
        const json entries = json::parse(out.str());
        ASSERT_EQ(entries.size(), 2u);
        EXPECT_EQ(entries[0].at("size"), 1);
        EXPECT_TRUE(entries[1].contains("point_geom"));
    }
}
