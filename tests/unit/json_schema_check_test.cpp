/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The JSON Schema subset checker of the contract tests (json_schema_check.h): each keyword
// accepts what it should and reports what it should not, so a contract schema is enforced.

#include <fstream>
#include <stdexcept>
#include <string>

#include "gtest/gtest.h"
#include "json.h"
#include "json_schema_check.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using testutils::JsonSchemaCheck;

    class SchemaCheck : public ::testing::Test
    {
    protected:
        std::unique_ptr<TestArea> ta;
        std::unique_ptr<JsonSchemaCheck> check;

        void SetUp() override
        {
            ta = std::make_unique<TestArea>(TEST_NAME, true);
            check = std::make_unique<JsonSchemaCheck>(ta->getFolder("schemas"));
        }

        void schema(const std::string &file, const json &content)
        {
            std::ofstream(ta->getFolder("schemas") / file) << content.dump();
        }

        bool valid(const json &instance, const std::string &file = "s.schema.json")
        {
            return check->validate(instance, file).empty();
        }
    };

    TEST_F(SchemaCheck, Types)
    {
        schema("s.schema.json", {{"type", json::array({"integer", "null"})}});
        EXPECT_TRUE(valid(5));
        EXPECT_TRUE(valid(-5));
        EXPECT_TRUE(valid(nullptr));
        EXPECT_FALSE(valid(5.0)) << "an integer is a JSON integer literal";
        EXPECT_FALSE(valid("5"));

        schema("n.schema.json", {{"type", "number"}});
        EXPECT_TRUE(valid(5, "n.schema.json"));
        EXPECT_TRUE(valid(5.5, "n.schema.json"));
        EXPECT_FALSE(valid(true, "n.schema.json"));
    }

    TEST_F(SchemaCheck, Objects)
    {
        schema("s.schema.json", json::parse(R"({"type": "object", "required": ["a"],
            "properties": {"a": {"type": "string"}, "b": {"type": "boolean"}},
            "additionalProperties": false})"));
        EXPECT_TRUE(valid({{"a", "x"}}));
        EXPECT_TRUE(valid({{"a", "x"}, {"b", true}}));
        EXPECT_FALSE(valid({{"b", true}})) << "required";
        EXPECT_FALSE(valid({{"a", 1}})) << "property type";
        EXPECT_FALSE(valid({{"a", "x"}, {"c", 1}})) << "additionalProperties: false";

        schema("m.schema.json", json::parse(R"({"type": "object", "additionalProperties": {"type": "integer"}})"));
        EXPECT_TRUE(valid({{"1", 2}}, "m.schema.json"));
        EXPECT_FALSE(valid({{"1", "2"}}, "m.schema.json"));
    }

    TEST_F(SchemaCheck, Arrays)
    {
        schema("s.schema.json", json::parse(R"({"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 3})"));
        EXPECT_TRUE(valid({1, 2.5}));
        EXPECT_FALSE(valid({1}));
        EXPECT_FALSE(valid({1, 2, 3, 4}));
        EXPECT_FALSE(valid({1, "2"}));
    }

    TEST_F(SchemaCheck, ValuesAndStrings)
    {
        schema("s.schema.json", json::parse(R"({"enum": ["a", 1]})"));
        EXPECT_TRUE(valid("a"));
        EXPECT_TRUE(valid(1));
        EXPECT_FALSE(valid("b"));

        schema("c.schema.json", json::parse(R"({"const": "Feature"})"));
        EXPECT_TRUE(valid("Feature", "c.schema.json"));
        EXPECT_FALSE(valid("Collection", "c.schema.json"));

        schema("p.schema.json", json::parse(R"({"type": "string", "pattern": "^[0-9a-f]{4}$", "minLength": 4})"));
        EXPECT_TRUE(valid("00af", "p.schema.json"));
        EXPECT_FALSE(valid("00AF", "p.schema.json"));

        schema("r.schema.json", json::parse(R"({"type": "number", "minimum": 0, "maximum": 1})"));
        EXPECT_TRUE(valid(0.5, "r.schema.json"));
        EXPECT_FALSE(valid(-0.1, "r.schema.json"));
        EXPECT_FALSE(valid(2, "r.schema.json"));
    }

    TEST_F(SchemaCheck, Combinators)
    {
        schema("s.schema.json", json::parse(R"({"oneOf": [{"type": "integer"}, {"type": "number"}]})"));
        EXPECT_TRUE(valid(1.5));
        EXPECT_FALSE(valid(1)) << "an integer is also a number: two branches match";

        schema("a.schema.json", json::parse(R"({"anyOf": [{"type": "string"}, {"type": "null"}]})"));
        EXPECT_TRUE(valid(nullptr, "a.schema.json"));
        EXPECT_FALSE(valid(1, "a.schema.json"));

        schema("all.schema.json", json::parse(R"({"allOf": [{"type": "integer"}, {"minimum": 3}]})"));
        EXPECT_TRUE(valid(3, "all.schema.json"));
        EXPECT_FALSE(valid(2, "all.schema.json"));
    }

    TEST_F(SchemaCheck, References)
    {
        schema("defs.schema.json", json::parse(R"({"$defs": {"id": {"type": "string", "minLength": 1}}, "type": "boolean"})"));
        schema("s.schema.json", json::parse(R"({"type": "object", "properties": {
            "id": {"$ref": "defs.schema.json#/$defs/id"},
            "flag": {"$ref": "defs.schema.json"},
            "local": {"$ref": "#/$defs/n"}}, "$defs": {"n": {"type": "integer"}}})"));
        EXPECT_TRUE(valid({{"id", "x"}, {"flag", true}, {"local", 1}}));
        EXPECT_FALSE(valid({{"id", ""}}));
        EXPECT_FALSE(valid({{"flag", 1}}));
        EXPECT_FALSE(valid({{"local", "1"}}));
    }

    TEST_F(SchemaCheck, ErrorsNameTheLocation)
    {
        schema("s.schema.json", json::parse(R"({"type": "array", "items": {"type": "object",
            "properties": {"a/b": {"type": "integer"}}}})"));
        const auto errors = check->validate(json::parse(R"([{"a/b": 1}, {"a/b": 1.5}])"), "s.schema.json");
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_EQ(errors[0].rfind("/1/a~1b: ", 0), 0u) << errors[0];
    }

    // A keyword outside the subset would be ignored: it is an error instead
    TEST_F(SchemaCheck, UnsupportedKeywordThrows)
    {
        schema("s.schema.json", json::parse(R"({"type": "object", "if": {"required": ["a"]}})"));
        EXPECT_THROW(check->validate(json::object(), "s.schema.json"), std::runtime_error);

        schema("cycle.schema.json", json::parse(R"({"$ref": "#"})"));
        EXPECT_THROW(check->validate(1, "cycle.schema.json"), std::runtime_error);
    }
}
