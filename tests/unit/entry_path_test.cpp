/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// libddb's dataset path rules (entry_path.h) and DDBNormalizePath. Every golden vector of
// tests/contract/vectors/entry-paths.json runs through the C++ function and, unless it holds a
// NUL ("cString": false), through the C API; Registry runs the same file against
// DDBNormalizePath. The existing skips of the indexer (".ddb" segments, names with '\') call
// the same helpers.

#include "entry_path.h"

#include <fstream>
#include <set>
#include <string>

#include "contract_fixtures.h"
#include "dbops.h"
#include "ddb.h"
#include "gtest/gtest.h"
#include "json.h"
#include "json_schema_check.h"
#include "utf8.h"

namespace {
using namespace ddb;

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

// Expects the call to fail with DDBERR_INVALIDARGS (DDBNormalizePath is an export new in 2.0) and a
// message of its own in DDBGetLastError()
#define EXPECT_DDB_INVALIDARGS(call)                    \
    do {                                                \
        DDBSetLastError("stale");                       \
        EXPECT_EQ((call), DDBERR_INVALIDARGS) << #call; \
        EXPECT_STRNE(DDBGetLastError(), "") << #call;   \
        EXPECT_STRNE(DDBGetLastError(), "stale") << #call; \
    } while (0)

json readVectors() {
    std::ifstream in(contract::contractDir() / "vectors" / "entry-paths.json", std::ios::binary);
    EXPECT_TRUE(in.good());
    return json::parse(in);
}

// DDBNormalizePath's output as JSON (reservedJson: NULL for none)
json normalizeWithCApi(const std::string& path, const char* reservedJson) {
    char* out = nullptr;
    EXPECT_EQ(DDBNormalizePath(path.c_str(), reservedJson, &out), DDBERR_NONE)
        << path << ": " << DDBGetLastError();
    if (out == nullptr)
        return nullptr;
    const json result = json::parse(out);
    DDBFree(out);
    return result;
}

TEST(EntryPathVectors, everyVectorMatchesInCppAndThroughTheCApi) {
    const json doc = readVectors();
    ASSERT_TRUE(doc.contains("vectors"));
    testutils::JsonSchemaCheck check(contract::contractDir() / "schemas");
    const std::set<std::string> allowedKeys = {"input", "reserved", "cString", "expected", "note"};

    std::set<std::string> codes;
    size_t throughCApi = 0;
    for (const auto& vector : doc.at("vectors")) {
        const auto input = vector.at("input").get<std::string>();
        SCOPED_TRACE(
            "input: " + json(input).dump() +
            (vector.contains("reserved") ? " reserved: " + vector["reserved"].dump() : ""));
        for (const auto& [key, value] : vector.items())
            EXPECT_EQ(allowedKeys.count(key), 1u) << "unknown key " << key;

        const json& expected = vector.at("expected");
        const auto schemaErrors = check.validate(expected, "normalize-path.schema.json");
        EXPECT_TRUE(schemaErrors.empty()) << json(schemaErrors).dump(2);
        if (expected.contains("rejected"))
            codes.insert(expected["rejected"].get<std::string>());

        const std::string reservedJson =
            vector.contains("reserved") ? vector["reserved"].dump() : std::string();
        const auto rules = ReservedRules::fromJson(reservedJson.c_str());
        EXPECT_EQ(json(normalizeEntryPath(input, rules)), expected);

        const bool cString = vector.value("cString", true);
        EXPECT_EQ(cString, input.find('\0') == std::string::npos)
            << "cString must be false exactly for the inputs with a NUL";
        if (cString) {
            const json out =
                normalizeWithCApi(input, reservedJson.empty() ? nullptr : reservedJson.c_str());
            EXPECT_EQ(out, expected);
            EXPECT_TRUE(check.validate(out, "normalize-path.schema.json").empty());
            throughCApi++;
        }
    }
    EXPECT_GT(throughCApi, 40u);
    // Every code but "encoding" (a JSON string cannot hold invalid UTF-8: see below)
    EXPECT_EQ(codes, (std::set<std::string>{"empty", "nul", "absolute", "traversal", "reserved"}));
}

TEST(EntryPath, rejectsInvalidUtf8) {
    for (const std::string bad :
         {"\xFF", "a/\xC3", "\xC0\xAF", "\xED\xA0\x80/x", "\xF4\x90\x80\x80", "a\x80"}) {
        SCOPED_TRACE(json(std::vector<int>(bad.begin(), bad.end())).dump());
        EXPECT_FALSE(utf8::isValid(bad));
        EXPECT_EQ(normalizeEntryPath(bad).rejected, "encoding");
        EXPECT_EQ(normalizeWithCApi(bad, nullptr), json({{"rejected", "encoding"}}));
    }
    // The longest valid sequences of each length
    EXPECT_TRUE(utf8::isValid("\x7F\xDF\xBF\xEF\xBF\xBF\xF4\x8F\xBF\xBF"));
    EXPECT_EQ(normalizeEntryPath("\xF4\x8F\xBF\xBF").path, "\xF4\x8F\xBF\xBF");
}

TEST(EntryPath, reservedSegmentIsDotDdbAsAFileSystemOpensIt) {
    for (const char* reserved : {".ddb", ".DDB", ".Ddb", ".ddb.", ".ddb ", ".ddb. .", ".ddb::$INDEX_ALLOCATION",
                                 ".DDB:stream"}) {
        EXPECT_TRUE(isReservedSegment(std::string_view(reserved))) << reserved;
        EXPECT_TRUE(isReservedSegment(fs::path(reserved))) << reserved;
    }
    for (const char* ordinary : {".ddbx", ".DDBX", ".ddb_notes.txt", "x.ddb", "", ".ddb/", " .ddb", ".ddb.x",
                                 "ddb", ".", "..", "x:.ddb"}) {
        EXPECT_FALSE(isReservedSegment(std::string_view(ordinary))) << ordinary;
        EXPECT_FALSE(isReservedSegment(fs::path(ordinary))) << ordinary;
    }
    // containsDdbSegment (the indexer, glob expansion, status) applies the same rule per component
    EXPECT_TRUE(containsDdbSegment(fs::path("a") / ".ddb" / "b"));
    EXPECT_TRUE(containsDdbSegment(fs::path("a") / ".DDB" / "b"));
    EXPECT_TRUE(containsDdbSegment(fs::path("a") / ".ddb " / "b"));
    EXPECT_FALSE(containsDdbSegment(fs::path("a.ddbfile.txt")));

    EXPECT_TRUE(isEntryPathSeparator('/'));
    EXPECT_TRUE(isEntryPathSeparator('\\'));
    EXPECT_FALSE(isEntryPathSeparator(':'));
}

// NTFS gives ".ddb" an 8.3 short name, "DDB~<n>", that opens the folder too: reserved on every
// platform, in any case and with the same trailing dots, spaces and stream names dropped, while a
// name that is no short name of ".ddb" stays ordinary
TEST(EntryPath, theShortNameOfDotDdbIsReserved) {
    for (const char* reserved : {"DDB~1", "ddb~1", "Ddb~2", "DDB~12", "ddb~0123456789", "DDB~1.", "DDB~1 ",
                                 "DDB~1::$INDEX_ALLOCATION"}) {
        EXPECT_TRUE(isReservedSegment(std::string_view(reserved))) << reserved;
        EXPECT_TRUE(isReservedSegment(fs::path(reserved))) << reserved;
    }
    for (const char* ordinary : {"ddb", "ddb~", "ddb~x", "ddb~1x", "ddb~-1", "ddb~1.txt", "xddb~1", ".ddb~1", "ddb~ 1",
                                 "DDB1", "ddb_1"}) {
        EXPECT_FALSE(isReservedSegment(std::string_view(ordinary))) << ordinary;
        EXPECT_FALSE(isReservedSegment(fs::path(ordinary))) << ordinary;
    }
    EXPECT_TRUE(containsDdbSegment(fs::path("a") / "DDB~1" / "build"));
    EXPECT_EQ(normalizeEntryPath("DDB~1/build/x.tif", ReservedRules()).rejected, "reserved");
}

TEST(EntryPath, reservedJsonIsAnArrayOfSegments) {
    EXPECT_TRUE(ReservedRules::fromJson(nullptr).firstSegment.empty());
    EXPECT_TRUE(ReservedRules::fromJson("").firstSegment.empty());
    EXPECT_TRUE(ReservedRules::fromJson("[]").firstSegment.empty());
    EXPECT_EQ(ReservedRules::fromJson(R"([".uploads", "x"])").firstSegment,
              (std::vector<std::string>{".uploads", "x"}));

    char* out = nullptr;
    for (const char* invalid : {"not json",
                                "{}",
                                "\".uploads\"",
                                "[1]",
                                "[\"\"]",
                                "[\".\"]",
                                "[\"..\"]",
                                "[\"a/b\"]",
                                "[\"a\\\\b\"]",
                                "[\"/a\"]",
                                "[\"a\\u0000\"]"}) {
        SCOPED_TRACE(invalid);
        EXPECT_THROW(ReservedRules::fromJson(invalid), InvalidArgsException);
        EXPECT_DDB_INVALIDARGS(DDBNormalizePath("a", invalid, &out));
        EXPECT_EQ(out, nullptr);
    }
}

TEST(EntryPath, cApiArguments) {
    char* out = nullptr;
    EXPECT_DDB_INVALIDARGS(DDBNormalizePath(nullptr, nullptr, &out));
    EXPECT_DDB_INVALIDARGS(DDBNormalizePath("a", nullptr, nullptr));
    EXPECT_EQ(out, nullptr);

    // A rejection is a result, not an error: the last error is left alone
    DDBSetLastError("");
    ASSERT_DDB_OK(DDBNormalizePath("../x", "[\".uploads\"]", &out));
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, R"({"rejected":"traversal"})");
    EXPECT_STREQ(DDBGetLastError(), "");
    DDBFree(out);
}
}  // namespace
