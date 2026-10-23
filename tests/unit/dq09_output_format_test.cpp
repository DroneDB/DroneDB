/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-09: OutputFormat and the one JSON array writer.

#include <sstream>

#include "gtest/gtest.h"

#include "database.h"
#include "dbops.h"
#include "delta.h"
#include "exceptions.h"
#include "info.h"
#include "output_format.h"
#include "test.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    TEST(Dq09OutputFormat, NamesRoundTrip)
    {
        for (const auto format : {OutputFormat::Text, OutputFormat::Json, OutputFormat::GeoJson})
            EXPECT_EQ(findOutputFormat(outputFormatName(format)), format);
        EXPECT_FALSE(findOutputFormat("xml"));
        EXPECT_FALSE(findOutputFormat("JSON")) << "names are lowercase, as the CLI always required";
        EXPECT_FALSE(findOutputFormat(""));
    }

    TEST(Dq09OutputFormat, ParsingChecksTheAllowedFormats)
    {
        EXPECT_EQ(parseOutputFormat("geojson"), OutputFormat::GeoJson);
        EXPECT_EQ(parseOutputFormat("text", {OutputFormat::Text, OutputFormat::Json}), OutputFormat::Text);
        try
        {
            parseOutputFormat("geojson", {OutputFormat::Text, OutputFormat::Json});
            FAIL() << "geojson is not allowed here";
        }
        catch (const InvalidArgsException &e)
        {
            EXPECT_STREQ(e.what(), "Invalid format geojson");
        }
        EXPECT_THROW(parseOutputFormat("xml"), InvalidArgsException);
        EXPECT_THROW(requireOutputFormat(OutputFormat::GeoJson, {OutputFormat::Text}), InvalidArgsException);
        EXPECT_NO_THROW(requireOutputFormat(OutputFormat::Text, {OutputFormat::Text}));
    }

    TEST(Dq09OutputFormat, TheJsonArrayWriterSeparatesTheMembersAndClosesTheArray)
    {
        std::ostringstream out;
        JsonArrayWriter writer(out);
        EXPECT_EQ(out.str(), "[");
        writer.write(json{{"a", 1}});
        writer.write("two");
        writer.write(json::array({3}));
        writer.finish();
        EXPECT_EQ(out.str(), R"([{"a":1},"two",[3]])");

        std::ostringstream empty;
        JsonArrayWriter none(empty);
        none.finish();
        EXPECT_EQ(empty.str(), "[]");

        std::ostringstream wrapped;
        JsonArrayWriter collection(wrapped, R"({"features":[)", "]}");
        collection.write(1);
        collection.write(2);
        collection.finish();
        EXPECT_EQ(wrapped.str(), R"({"features":[1,2]})");
    }

    TEST(Dq09OutputFormat, EntriesAreWrittenAsTextLinesOrAJsonArray)
    {
        Entry a, b;
        a.path = "a.txt";
        a.type = EntryType::Generic;
        b.path = "dir/b.txt";
        b.type = EntryType::Generic;
        b.hash = "h";

        std::ostringstream text, asJson, decorated;
        writeEntries(text, {a, b}, OutputFormat::Text);
        writeEntries(asJson, {a, b}, OutputFormat::Json);
        writeEntries(decorated, {a}, OutputFormat::Json, [](const Entry &, json &j) { j["extra"] = true; });

        EXPECT_EQ(text.str(), "a.txt\ndir/b.txt\n");
        EXPECT_EQ(asJson.str(), json::array({json(a), json(b)}).dump());
        EXPECT_EQ(json::parse(decorated.str())[0].at("extra"), true);

        std::ostringstream none;
        writeEntries(none, {}, OutputFormat::Json);
        EXPECT_EQ(none.str(), "[]");

        std::ostringstream rejected;
        EXPECT_THROW(writeEntries(rejected, {a}, OutputFormat::GeoJson), InvalidArgsException);
        EXPECT_TRUE(rejected.str().empty());
    }

    TEST(Dq09OutputFormat, ListAndSearchTakeTheEnum)
    {
        IndexFixture f(TEST_NAME);
        writeFile(f.root / "a.txt", "alpha");
        writeFile(f.root / "d" / "b.txt", "beta");
        addToIndex(f.db.get(), {f.root.string()});

        std::ostringstream byEnum;
        searchIndex(f.db.get(), "*.txt", byEnum, OutputFormat::Text);
        EXPECT_EQ(byEnum.str(), "a.txt\nd/b.txt\n");

        std::ostringstream listed;
        listIndex(f.db.get(), {f.root.string()}, listed, OutputFormat::Json);
        EXPECT_EQ(json::parse(listed.str()).size(), 2u);

        std::ostringstream out;
        EXPECT_THROW(searchIndex(f.db.get(), "*", out, OutputFormat::GeoJson), InvalidArgsException);
        EXPECT_THROW(listIndex(f.db.get(), {f.root.string()}, out, OutputFormat::GeoJson), InvalidArgsException);
        EXPECT_TRUE(out.str().empty());
    }

    TEST(Dq09OutputFormat, DeltaWritesTextOrJson)
    {
        const json source = {{"entries", json::array({json{{"a.txt", "h1"}}})}, {"meta", json::array()}, {"checksum", "x"}};
        const json target = {{"entries", json::array()}, {"meta", json::array()}, {"checksum", "y"}};

        std::ostringstream text, asJson;
        delta(source, target, text, OutputFormat::Text);
        delta(source, target, asJson, OutputFormat::Json);
        EXPECT_EQ(text.str(), "A\ta.txt\n");
        EXPECT_TRUE(json::parse(asJson.str()).contains("adds"));

        std::ostringstream rejected;
        EXPECT_THROW(delta(source, target, rejected, OutputFormat::GeoJson), InvalidArgsException);
    }
}
