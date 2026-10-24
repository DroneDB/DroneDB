/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-09 (XI-09): the exports that take a dataset-relative entry path apply the rule of
// DDBNormalizePath (entry_path.h) to it, and refuse a path outside the dataset or in the reserved
// ".ddb" folder with DDBERR_INVALIDARGS, also the exports of 1.x (whose other invalid arguments
// stay DDBERR_EXCEPTION).
//
// Every export that takes an entry path runs the golden vectors of
// tests/contract/vectors/entry-paths.json (the ones without extra reserved names, which the C
// API does not take):
//  - an accepted vector reaches the entry at its normalized spelling, whatever the input spelling;
//  - a refused vector returns DDBERR_INVALIDARGS with the rejection code in the message and an
//    empty reason, and changes nothing.
// The exports with a filesystem path (a file being added, an output file, a dataset root) are
// not entry paths and keep their paths: DDBAdd, DDBRemove, DDBList, DDBInfo (see ddb.h).

#include <cctype>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "contract_fixtures.h"
#include "ddb.h"
#include "delta.h"
#include "delta_apply.h"
#include "entry_path.h"
#include "gtest/gtest.h"
#include "hash.h"
#include "json.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;
namespace fs = std::filesystem;

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

// A string returned by the library, copied and freed
std::string take(char* s) {
    std::string copy = s != nullptr ? s : "";
    EXPECT_EQ(DDBFree(s), DDBERR_NONE);
    return copy;
}

// One golden vector the C API applies (no extra reserved names)
struct PathVector {
    std::string input;
    bool accepted = false;
    std::string normalized;  // when accepted
    std::string rejected;    // the code, when not
    bool blank = false;  // "" or whitespace only: the exports with a required path say "no path"
};

std::vector<PathVector> loadVectors() {
    std::ifstream in(contract::contractDir() / "vectors" / "entry-paths.json", std::ios::binary);
    EXPECT_TRUE(in.good());
    const json doc = json::parse(in);
    std::vector<PathVector> vectors;
    for (const auto& v : doc.at("vectors")) {
        if (v.contains("reserved") || !v.value("cString", true))
            continue;
        PathVector p;
        p.input = v.at("input").get<std::string>();
        const auto& expected = v.at("expected");
        p.accepted = expected.contains("path");
        if (p.accepted)
            p.normalized = expected["path"].get<std::string>();
        else
            p.rejected = expected["rejected"].get<std::string>();
        p.blank = std::all_of(p.input.begin(), p.input.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        });
        vectors.push_back(std::move(p));
    }
    return vectors;
}

// Whether a file can be created at the path on every platform the tests run on (Windows names:
// no ':' and the like, no trailing space or dot, no device names), and cannot be the ".ddb"
// folder of the dataset on a case-insensitive file system
bool creatableEverywhere(const std::string& path) {
    static const std::set<std::string> deviceNames = {"con", "prn", "aux", "nul", "com1", "lpt1"};
    size_t start = 0;
    while (start <= path.size()) {
        const auto end = std::min(path.find('/', start), path.size());
        const auto segment = path.substr(start, end - start);
        std::string lower;
        for (const unsigned char c : segment)
            lower += static_cast<char>(std::tolower(c));
        if (segment.find_first_of("<>:\"|?*\\") != std::string::npos || segment.empty() ||
            segment.back() == ' ' || segment.back() == '.' || lower == DDB_FOLDER ||
            deviceNames.count(lower) > 0)
            return false;
        start = end + 1;
    }
    return true;
}

// A dataset at <area>/ds with "keep.txt", and, when path is given, the file `path`, indexed
struct Dataset {
    TestArea area;
    fs::path root;

    explicit Dataset(const std::string& name, const std::string& path = "")
        : area(name, true), root(area.getFolder("ds")) {
        char* out = nullptr;
        EXPECT_EQ(DDBInit(root.string().c_str(), &out), DDBERR_NONE) << DDBGetLastError();
        take(out);
        add("keep.txt");
        if (!path.empty())
            add(path);
    }

    void add(const std::string& rel) {
        const fs::path file = root / io::fromUtf8(rel);
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << "content of " << rel;
        const std::string abs = io::toUtf8(file);
        const char* paths[] = {abs.c_str()};
        char* out = nullptr;
        ASSERT_EQ(DDBAdd(path().c_str(), paths, 1, &out, false), DDBERR_NONE) << DDBGetLastError();
        take(out);
    }

    std::string path() const { return root.string(); }

    // The paths of every entry, as DDBListEntries lists them
    std::vector<std::string> entries() const {
        char* out = nullptr;
        EXPECT_EQ(DDBListEntries(path().c_str(), "", true, "", "", false, &out), DDBERR_NONE)
            << DDBGetLastError();
        std::vector<std::string> paths;
        for (const auto& e : json::parse(take(out)))
            paths.push_back(e["path"].get<std::string>());
        return paths;
    }
};

// What an empty path means to an export
enum class Blank {
    Required,  // "" and whitespace: "No path provided" (DDBERR_EXCEPTION, 1.x), not a refused path
    Optional,  // "" is none (the dataset, or all); whitespace is a name
    Folder,    // "" and "." name the dataset root
    List,      // an element names an entry: "" is refused; whitespace is a name
};

// An export under test: a call with the path in its entry path argument, and what it must do to
// the entry of an accepted path (check: the call succeeded; verify: the effect, on the dataset)
struct PathExport {
    std::string name;
    Blank blank;
    std::function<DDBErr(const Dataset&, const std::string& path)> call;
    // Run after a successful call on a dataset that has the file `normalized` (and "keep.txt")
    std::function<void(const Dataset&, const std::string& normalized)> verify;
};

// The path of the first item of a JSON array (or object) output, "" when there is none
std::string firstPath(const std::string& text) {
    const json j = json::parse(text);
    if (j.is_array())
        return j.empty() ? "" : j[0].value("path", "");
    return j.value("path", "");
}

// Calls an export with a string output and keeps the output of the last call
struct Output {
    std::string text;
};

// Sets the "note" key on the entry (or the dataset)
DDBErr setNote(const Dataset& ds, const std::string& path) {
    char* out = nullptr;
    const DDBErr rc = DDBMetaSet(ds.path().c_str(), path.c_str(), "note", "\"n\"", &out);
    take(out);
    return rc;
}

// The answer of a boolean export for the path as given and for its normalized spelling: the same
std::function<DDBErr(const Dataset&, const std::string&)> sameBooleanForTheNormalizedPath(
    const std::function<DDBErr(const Dataset&, const std::string&, bool*)>& ask) {
    return [ask](const Dataset& ds, const std::string& p) {
        bool given = false;
        const DDBErr rc = ask(ds, p, &given);
        if (rc == DDBERR_NONE) {
            // The same question about the normalized spelling answers the same
            bool normalized = !given;
            EXPECT_EQ(ask(ds, normalizeEntryPath(p).path, &normalized), DDBERR_NONE);
            EXPECT_EQ(given, normalized) << p;
        }
        return rc;
    };
}

std::vector<PathExport> pathExports(Output& last) {
    const auto keep = [&last](char* out) { last.text = take(out); };
    std::vector<PathExport> exports;

    exports.push_back(
        {"DDBGet",
         Blank::Required,
         [=](const Dataset& ds, const std::string& p) {
             char* out = nullptr;
             const DDBErr rc = DDBGet(ds.path().c_str(), p.c_str(), &out);
             keep(out);
             return rc;
         },
         [&last](const Dataset&, const std::string& n) { EXPECT_EQ(firstPath(last.text), n); }});

    exports.push_back(
        {"DDBListEntries",
         Blank::Folder,
         [=](const Dataset& ds, const std::string& p) {
             char* out = nullptr;
             const DDBErr rc =
                 DDBListEntries(ds.path().c_str(), p.c_str(), true, "", "", false, &out);
             keep(out);
             return rc;
         },
         [&last](const Dataset&, const std::string& n) { EXPECT_EQ(firstPath(last.text), n); }});

    exports.push_back(
        {"DDBListEntriesEx",
         Blank::Folder,
         [=](const Dataset& ds, const std::string& p) {
             char* out = nullptr;
             DDBListEntriesOptions options{};
             options.structSize = sizeof(options);
             options.recursive = true;
             const DDBErr rc =
                 DDBListEntriesEx(ds.path().c_str(), p.c_str(), "", "", &options, &out);
             keep(out);
             return rc;
         },
         [&last](const Dataset&, const std::string& n) { EXPECT_EQ(firstPath(last.text), n); }});

    exports.push_back(
        {"DDBSearch",
         Blank::Optional,
         [=](const Dataset& ds, const std::string& p) {
             char* out = nullptr;
             const DDBErr rc = DDBSearch(ds.path().c_str(), p.c_str(), &out, "json");
             keep(out);
             return rc;
         },
         [&last](const Dataset&, const std::string& n) { EXPECT_EQ(firstPath(last.text), n); }});

    exports.push_back({"DDBMetaAdd",
                       Blank::Optional,
                       [=](const Dataset& ds, const std::string& p) {
                           char* out = nullptr;
                           const DDBErr rc =
                               DDBMetaAdd(ds.path().c_str(), p.c_str(), "notes", "\"n\"", &out);
                           keep(out);
                           return rc;
                       },
                       [](const Dataset& ds, const std::string& n) {
                           char* out = nullptr;
                           ASSERT_DDB_OK(DDBMetaList(ds.path().c_str(), n.c_str(), &out));
                           EXPECT_EQ(json::parse(take(out)).size(), 1u);
                       }});

    exports.push_back({"DDBMetaSet",
                       Blank::Optional,
                       [=](const Dataset& ds, const std::string& p) {
                           char* out = nullptr;
                           const DDBErr rc =
                               DDBMetaSet(ds.path().c_str(), p.c_str(), "note", "\"n\"", &out);
                           keep(out);
                           return rc;
                       },
                       [](const Dataset& ds, const std::string& n) {
                           char* out = nullptr;
                           ASSERT_DDB_OK(DDBMetaGet(ds.path().c_str(), n.c_str(), "note", &out));
                           take(out);
                       }});

    exports.push_back({"DDBMetaGet",
                       Blank::Optional,
                       [=](const Dataset& ds, const std::string& p) {
                           // The entry (or the dataset) holds the key
                           char* out = nullptr;
                           if (const DDBErr set = setNote(ds, p); set != DDBERR_NONE)
                               return set;
                           const DDBErr rc = DDBMetaGet(ds.path().c_str(), p.c_str(), "note", &out);
                           keep(out);
                           return rc;
                       },
                       [&last](const Dataset&, const std::string&) {
                           EXPECT_EQ(json::parse(last.text).value("data", ""), "n");
                       }});

    exports.push_back({"DDBMetaUnset",
                       Blank::Optional,
                       [=](const Dataset& ds, const std::string& p) {
                           char* out = nullptr;
                           if (const DDBErr set = setNote(ds, p); set != DDBERR_NONE)
                               return set;
                           const DDBErr rc =
                               DDBMetaUnset(ds.path().c_str(), p.c_str(), "note", &out);
                           keep(out);
                           return rc;
                       },
                       [&last](const Dataset&, const std::string&) {
                           EXPECT_EQ(json::parse(last.text).value("removed", 0), 1);
                       }});

    exports.push_back({"DDBMetaList",
                       Blank::Optional,
                       [=](const Dataset& ds, const std::string& p) {
                           char* out = nullptr;
                           if (const DDBErr set = setNote(ds, p); set != DDBERR_NONE)
                               return set;
                           const DDBErr rc = DDBMetaList(ds.path().c_str(), p.c_str(), &out);
                           keep(out);
                           return rc;
                       },
                       [&last](const Dataset&, const std::string& n) {
                           ASSERT_FALSE(json::parse(last.text).empty());
                           EXPECT_EQ(firstPath(last.text), n);
                       }});

    exports.push_back({"DDBMoveEntry",
                       Blank::Required,
                       [](const Dataset& ds, const std::string& p) {
                           return DDBMoveEntry(ds.path().c_str(), p.c_str(), "moved.txt");
                       },
                       [](const Dataset& ds, const std::string& n) {
                           char* out = nullptr;
                           ASSERT_DDB_OK(DDBGet(ds.path().c_str(), "moved.txt", &out));
                           take(out);
                           EXPECT_EQ(DDBGet(ds.path().c_str(), n.c_str(), &out), DDBERR_NOTFOUND);
                       }});

    exports.push_back({"DDBMoveEntry (dest)",
                       Blank::Required,
                       [](const Dataset& ds, const std::string& p) {
                           return DDBMoveEntry(ds.path().c_str(), "keep.txt", p.c_str());
                       },
                       [](const Dataset& ds, const std::string& n) {
                           char* out = nullptr;
                           ASSERT_DDB_OK(DDBGet(ds.path().c_str(), n.c_str(), &out));
                           take(out);
                           EXPECT_EQ(DDBGet(ds.path().c_str(), "keep.txt", &out), DDBERR_NOTFOUND);
                       }});

    exports.push_back({"DDBBuild",
                       Blank::Optional,
                       [](const Dataset& ds, const std::string& p) {
                           return DDBBuild(ds.path().c_str(), p.c_str(), nullptr, false, false);
                       },
                       nullptr});

    exports.push_back(
        {"DDBIsBuildable",
         Blank::Required,
         sameBooleanForTheNormalizedPath([](const Dataset& ds, const std::string& p, bool* out) {
             return DDBIsBuildable(ds.path().c_str(), p.c_str(), out);
         }),
         nullptr});

    exports.push_back(
        {"DDBIsBuildActive",
         Blank::Required,
         sameBooleanForTheNormalizedPath([](const Dataset& ds, const std::string& p, bool* out) {
             return DDBIsBuildActive(ds.path().c_str(), p.c_str(), out);
         }),
         nullptr});

    exports.push_back(
        {"DDBIsBuildComplete",
         Blank::Required,
         sameBooleanForTheNormalizedPath([](const Dataset& ds, const std::string& p, bool* out) {
             return DDBIsBuildComplete(ds.path().c_str(), p.c_str(), out);
         }),
         nullptr});

    exports.push_back({"DDBGetBuildStatus",
                       Blank::List,
                       [=](const Dataset& ds, const std::string& p) {
                           const char* paths[] = {p.c_str()};
                           char* out = nullptr;
                           const DDBErr rc = DDBGetBuildStatus(ds.path().c_str(), paths, 1, &out);
                           keep(out);
                           return rc;
                       },
                       [&last](const Dataset&, const std::string& n) {
                           EXPECT_EQ(firstPath(last.text), n);
                           EXPECT_TRUE(json::parse(last.text)[0].value("exists", false));
                       }});

    exports.push_back(
        {"DDBGetBuildArtifacts",
         Blank::List,
         [=](const Dataset& ds, const std::string& p) {
             const char* paths[] = {p.c_str()};
             char* out = nullptr;
             const DDBErr rc = DDBGetBuildArtifacts(ds.path().c_str(), paths, 1, &out);
             keep(out);
             return rc;
         },
         [&last](const Dataset&, const std::string& n) { EXPECT_EQ(firstPath(last.text), n); }});

    exports.push_back({"DDBStac",
                       Blank::Optional,
                       [=](const Dataset& ds, const std::string& p) {
                           char* out = nullptr;
                           const DDBErr rc =
                               DDBStac(ds.path().c_str(), p.c_str(), "", "", "", &out);
                           keep(out);
                           return rc;
                       },
                       nullptr});

    exports.push_back({"DDBStacData",
                       Blank::Optional,
                       [=](const Dataset& ds, const std::string& p) {
                           char* out = nullptr;
                           const DDBErr rc = DDBStacData(ds.path().c_str(), p.c_str(), &out);
                           keep(out);
                           return rc;
                       },
                       nullptr});

    return exports;
}

// An export that refuses a path is a refusal of the entry path, with the code of the rule
void expectRefused(const PathExport& e, const Dataset& ds, const PathVector& v) {
    SCOPED_TRACE(e.name + " " + json(v.input).dump());
    DDBSetLastError("stale");
    EXPECT_EQ(e.call(ds, v.input), DDBERR_INVALIDARGS);
    const std::string message = DDBGetLastError();
    EXPECT_NE(message.find(v.rejected), std::string::npos) << message;
    EXPECT_EQ(message.rfind("Invalid ", 0), 0u) << message;
    EXPECT_STREQ(DDBGetLastErrorReason(), "") << "a refused path is not retryable";
}

// Is the vector refused by this export (its empty path rule decides "" and ".")
bool refusedBy(const PathExport& e, const PathVector& v) {
    if (v.accepted)
        return false;
    if (v.input.empty() || v.blank)
        return false;  // "no path" or an empty one: see Blank
    if (v.rejected == "empty")
        return e.blank != Blank::Folder;  // "." and "./" name nothing, but the root as a folder
    return true;
}

TEST(CAbiEntryPath, everyRefusedVectorIsInvalidArgsInEveryExportAndChangesNothing) {
    const auto vectors = loadVectors();
    Output last;
    const auto exports = pathExports(last);
    size_t refused = 0;
    for (const auto& e : exports) {
        Dataset ds(TEST_NAME);
        const auto before = ds.entries();
        for (const auto& v : vectors) {
            if (!refusedBy(e, v))
                continue;
            expectRefused(e, ds, v);
            refused++;
        }
        EXPECT_EQ(ds.entries(), before) << e.name << " changed the dataset";
        // The meta of the dataset is untouched too
        char* out = nullptr;
        ASSERT_DDB_OK(DDBMetaList(ds.path().c_str(), "", &out));
        EXPECT_TRUE(json::parse(take(out)).empty()) << e.name;
    }
    EXPECT_GT(refused, 18u * 10u);
}

// An empty path keeps its meaning: none, the dataset root, or "no path provided" (a missing
// argument, not a refused path)
TEST(CAbiEntryPath, emptyPathKeepsItsMeaning) {
    Output last;
    const auto exports = pathExports(last);
    Dataset ds(TEST_NAME);
    for (const auto& e : exports) {
        SCOPED_TRACE(e.name);
        const DDBErr empty = e.call(ds, "");
        const std::string emptyMessage = DDBGetLastError();
        const DDBErr blank = e.call(ds, "  ");
        const std::string blankMessage = DDBGetLastError();
        switch (e.blank) {
            case Blank::Required:
                // A missing argument stays what it was for the exports of 1.x
                EXPECT_EQ(empty, DDBERR_EXCEPTION) << emptyMessage;
                EXPECT_EQ(blank, DDBERR_EXCEPTION) << blankMessage;
                break;
            case Blank::Optional:
            case Blank::Folder:
                EXPECT_EQ(empty, DDBERR_NONE) << emptyMessage;
                EXPECT_NE(blank, DDBERR_INVALIDARGS) << blankMessage;  // a name, not found
                break;
            case Blank::List:
                EXPECT_EQ(empty, DDBERR_INVALIDARGS) << emptyMessage;
                EXPECT_EQ(emptyMessage, "Invalid path '': empty");
                EXPECT_NE(blank, DDBERR_INVALIDARGS) << blankMessage;
                break;
        }
    }
    // "." and "./" are the dataset root for a folder
    for (const char* dot : {".", "./", ".\\"}) {
        char* out = nullptr;
        ASSERT_DDB_OK(DDBListEntries(ds.path().c_str(), dot, true, "", "", false, &out));
        EXPECT_EQ(firstPath(take(out)), "keep.txt") << dot;
    }
}

TEST(CAbiEntryPath, everyAcceptedVectorReachesTheEntryAtItsNormalizedSpelling) {
    const auto vectors = loadVectors();
    Output last;
    const auto exports = pathExports(last);
    size_t vectorsReached = 0;
    for (const auto& v : vectors) {
        if (!v.accepted || v.blank || !creatableEverywhere(v.normalized))
            continue;
        SCOPED_TRACE("input " + json(v.input).dump() + " -> " + v.normalized);
        vectorsReached++;
        for (const auto& e : exports) {
            SCOPED_TRACE(e.name);
            // The file exists only at the normalized spelling; "keep.txt" is the other entry
            Dataset ds(TEST_NAME, v.normalized);
            const auto indexed = ds.entries();
            ASSERT_NE(std::find(indexed.begin(), indexed.end(), v.normalized), indexed.end());

            const DDBErr rc = e.call(ds, v.input);
            ASSERT_EQ(rc, DDBERR_NONE) << DDBGetLastError();
            if (e.verify)
                e.verify(ds, v.normalized);
        }
    }
    EXPECT_GT(vectorsReached, 14u);
}

// A path that is accepted but names nothing is "not found", and says the normalized spelling
TEST(CAbiEntryPath, anAcceptedPathThatIsNotIndexedIsNotFoundUnderItsNormalizedSpelling) {
    const auto vectors = loadVectors();
    const Dataset ds(TEST_NAME);
    size_t checked = 0;
    for (const auto& v : vectors) {
        if (!v.accepted || v.blank)
            continue;
        char* out = nullptr;
        EXPECT_EQ(DDBGet(ds.path().c_str(), v.input.c_str(), &out), DDBERR_NOTFOUND) << v.input;
        EXPECT_EQ(std::string(DDBGetLastError()), "No entry " + v.normalized) << v.input;
        checked++;
    }
    EXPECT_GT(checked, 15u);
}

// "a\\b" and "a/./b/" are "a/b" for an existing entry: the path DDBGet reports is the entry's
TEST(CAbiEntryPath, separatorsAndDotSegmentsReachTheSameEntry) {
    const Dataset ds(TEST_NAME, "sub/dir/file.txt");
    for (const char* spelling :
         {"sub/dir/file.txt", "sub\\dir\\file.txt", "./sub//dir/./file.txt", "sub/dir/file.txt/"}) {
        char* out = nullptr;
        ASSERT_DDB_OK(DDBGet(ds.path().c_str(), spelling, &out));
        EXPECT_EQ(firstPath(take(out)), "sub/dir/file.txt") << spelling;
    }
    // A folder, with and without a trailing separator
    for (const char* spelling : {"sub/dir", "sub\\dir\\", ".\\sub/dir/"}) {
        char* out = nullptr;
        ASSERT_DDB_OK(DDBGet(ds.path().c_str(), spelling, &out));
        EXPECT_EQ(firstPath(take(out)), "sub/dir") << spelling;
    }
}

// Refusals with the messages a host shows
TEST(CAbiEntryPath, refusalNamesTheArgumentAndTheCode) {
    const Dataset ds(TEST_NAME);
    char* out = nullptr;
    EXPECT_EQ(DDBGet(ds.path().c_str(), "../x", &out), DDBERR_INVALIDARGS);
    EXPECT_STREQ(DDBGetLastError(), "Invalid path '../x': traversal");

    EXPECT_EQ(DDBMoveEntry(ds.path().c_str(), "keep.txt", ".ddb/dbase.sqlite"), DDBERR_INVALIDARGS);
    EXPECT_STREQ(DDBGetLastError(), "Invalid dest path '.ddb/dbase.sqlite': reserved");
    EXPECT_EQ(DDBMoveEntry(ds.path().c_str(), "/etc/passwd", "x"), DDBERR_INVALIDARGS);
    EXPECT_STREQ(DDBGetLastError(), "Invalid source path '/etc/passwd': absolute");

    EXPECT_EQ(DDBGet(ds.path().c_str(), "a/\xFF", &out), DDBERR_INVALIDARGS);
    EXPECT_STREQ(DDBGetLastError(), "Invalid path: encoding") << "invalid UTF-8 is not echoed";

    // Other invalid arguments of an export of 1.x stay DDBERR_EXCEPTION
    EXPECT_EQ(DDBGet(ds.path().c_str(), nullptr, &out), DDBERR_EXCEPTION);
    EXPECT_STREQ(DDBGetLastError(), "No path provided");
    EXPECT_EQ(DDBGet(nullptr, "keep.txt", &out), DDBERR_EXCEPTION);

    // The exports added in 2.0 return DDBERR_INVALIDARGS for both
    EXPECT_EQ(DDBStacData(ds.path().c_str(), "../x", &out), DDBERR_INVALIDARGS);
    EXPECT_EQ(DDBStacData(nullptr, "keep.txt", &out), DDBERR_INVALIDARGS);
}

// The list of paths is checked as a whole, before anything is read
TEST(CAbiEntryPath, oneRefusedPathRefusesTheList) {
    const Dataset ds(TEST_NAME);
    const char* paths[] = {"keep.txt", "sub/../keep.txt"};
    char* out = nullptr;
    EXPECT_EQ(DDBGetBuildStatus(ds.path().c_str(), paths, 2, &out), DDBERR_INVALIDARGS);
    EXPECT_EQ(DDBGetBuildArtifacts(ds.path().c_str(), paths, 2, &out), DDBERR_INVALIDARGS);
    const char* empty[] = {""};
    EXPECT_EQ(DDBGetBuildStatus(ds.path().c_str(), empty, 1, &out), DDBERR_INVALIDARGS);
}

// A search is a pattern over entry paths: '*' is an ordinary character of the rule
TEST(CAbiEntryPath, searchPatternsAreNormalized) {
    const Dataset ds(TEST_NAME, "photos/a.jpg");
    char* out = nullptr;
    ASSERT_DDB_OK(DDBSearch(ds.path().c_str(), "photos\\*.jpg", &out, "json"));
    EXPECT_EQ(firstPath(take(out)), "photos/a.jpg");
    ASSERT_DDB_OK(DDBSearch(ds.path().c_str(), "./photos//*", &out, "json"));
    EXPECT_EQ(firstPath(take(out)), "photos/a.jpg");
    ASSERT_DDB_OK(DDBSearch(ds.path().c_str(), "", &out, "json"));  // all
    EXPECT_EQ(json::parse(take(out)).size(), 3u);
    EXPECT_EQ(DDBSearch(ds.path().c_str(), "../*", &out, "json"), DDBERR_INVALIDARGS);
    EXPECT_EQ(DDBSearch(ds.path().c_str(), ".ddb/*", &out, "json"), DDBERR_INVALIDARGS);
}

// DDBBuild: the source is an entry path, the destination a folder of the filesystem
TEST(CAbiEntryPath, buildDestinationIsAFilesystemFolder) {
    Dataset ds(TEST_NAME);
    const std::string dest = ds.area.getFolder("elsewhere").string();
    ASSERT_DDB_OK(DDBBuild(ds.path().c_str(), "keep.txt", dest.c_str(), false, false));
    EXPECT_EQ(DDBBuild(ds.path().c_str(), "keep.txt/../x", dest.c_str(), false, false),
              DDBERR_INVALIDARGS);
}

// ------------------------------------------------------------------ the paths of a delta

// A dataset and a source folder with the files a delta adds
struct DeltaCase {
    Dataset ds;
    fs::path source;

    explicit DeltaCase(const std::string& name) : ds(name), source(ds.area.getFolder("source")) {}

    // Writes the file at `normalized` in the source folder; returns its hash
    std::string stage(const std::string& normalized) const {
        const fs::path file = source / io::fromUtf8(normalized);
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << "added " << normalized;
        return Hash::fileSHA256(io::toUtf8(file));
    }

    // The delta JSON that adds `rawPath`, the file staged at `normalized`, after the folders
    // above it (a delta lists a folder before its content)
    std::string addDelta(const std::string& rawPath, const std::string& normalized) const {
        Delta d;
        for (auto folder = fs::path(io::fromUtf8(normalized)).parent_path(); !folder.empty();
             folder = folder.parent_path())
            d.adds.insert(d.adds.begin(), AddAction(io::toGenericUtf8(folder), ""));
        d.adds.push_back(AddAction(rawPath, stage(normalized)));
        return json(d).dump();
    }

    // The hash of keep.txt, which the dataset has
    std::string keepHash() const { return Hash::fileSHA256(io::toUtf8(ds.root / "keep.txt")); }
};

TEST(CAbiEntryPath, deltaPathsAreRefusedBeforeAnythingChanges) {
    for (const auto& v : loadVectors()) {
        if (v.accepted || v.blank || v.input.empty())
            continue;
        SCOPED_TRACE(json(v.input).dump());
        DeltaCase c(TEST_NAME);
        Delta d;
        d.adds.push_back(AddAction("good.txt", c.stage("good.txt")));
        d.adds.push_back(AddAction(v.input, "h"));
        const std::string deltaJson = json(d).dump();
        const std::string source = c.source.string();
        const auto before = c.ds.entries();

        char* conflicts = nullptr;
        EXPECT_EQ(DDBApplyDelta(deltaJson.c_str(),
                                source.c_str(),
                                c.ds.path().c_str(),
                                MergeStrategy::KeepTheirs,
                                "[]",
                                &conflicts),
                  DDBERR_INVALIDARGS);
        EXPECT_NE(std::string(DDBGetLastError()).find(v.rejected), std::string::npos)
            << DDBGetLastError();
        EXPECT_EQ(DDBApplyDeltaEx(deltaJson.c_str(),
                                  source.c_str(),
                                  c.ds.path().c_str(),
                                  MergeStrategy::KeepTheirs,
                                  "[]",
                                  nullptr,
                                  &conflicts),
                  DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBComputeDeltaLocals(deltaJson.c_str(),
                                        c.ds.path().c_str(),
                                        c.ds.area.getFolder("links").string().c_str(),
                                        &conflicts),
                  DDBERR_INVALIDARGS);

        // Nothing was applied: not the valid add before it, nor a file under the links folder
        EXPECT_EQ(c.ds.entries(), before);
        EXPECT_FALSE(fs::exists(c.ds.root / "good.txt"));
        EXPECT_TRUE(fs::exists(c.source / "good.txt"));
        EXPECT_TRUE(fs::is_empty(c.ds.area.getFolder("links")));
    }
}

TEST(CAbiEntryPath, deltaPathsAreAppliedAtTheirNormalizedSpelling) {
    size_t applied = 0;
    for (const auto& v : loadVectors()) {
        if (!v.accepted || v.blank || !creatableEverywhere(v.normalized))
            continue;
        SCOPED_TRACE("input " + json(v.input).dump() + " -> " + v.normalized);
        DeltaCase c(TEST_NAME);
        const std::string deltaJson = c.addDelta(v.input, v.normalized);
        // The caller verified the file, under the spelling of its delta
        const std::string verified = json{{v.input, c.stage(v.normalized)}}.dump();
        const std::string source = c.source.string();

        char* conflicts = nullptr;
        ASSERT_DDB_OK(DDBApplyDeltaEx(deltaJson.c_str(),
                                      source.c_str(),
                                      c.ds.path().c_str(),
                                      MergeStrategy::KeepTheirs,
                                      "[]",
                                      verified.c_str(),
                                      &conflicts));
        EXPECT_EQ(take(conflicts), "[]");
        EXPECT_TRUE(fs::exists(c.ds.root / io::fromUtf8(v.normalized)));
        const auto entries = c.ds.entries();
        EXPECT_NE(std::find(entries.begin(), entries.end(), v.normalized), entries.end());
        applied++;
    }
    EXPECT_GT(applied, 14u);
}

// computeDeltaLocals writes hard links under a folder of the host: a delta path cannot leave it
TEST(CAbiEntryPath, localLinksStayUnderTheirFolder) {
    DeltaCase c(TEST_NAME);
    // keep.txt is in the dataset: a delta that adds the same content under a traversal path
    Delta d;
    d.adds.push_back(AddAction("../outside.txt", c.keepHash()));
    const std::string deltaJson = json(d).dump();
    const std::string links = c.ds.area.getFolder("links").string();
    char* out = nullptr;
    EXPECT_EQ(DDBComputeDeltaLocals(deltaJson.c_str(), c.ds.path().c_str(), links.c_str(), &out),
              DDBERR_INVALIDARGS);
    EXPECT_FALSE(fs::exists(c.ds.area.getPath("outside.txt")));
    EXPECT_FALSE(fs::exists(c.ds.area.getPath("links/../outside.txt")));

    // A valid path is linked under the folder
    Delta ok;
    ok.adds.push_back(AddAction("a\\b/./copy.txt", c.keepHash()));
    ASSERT_DDB_OK(
        DDBComputeDeltaLocals(json(ok).dump().c_str(), c.ds.path().c_str(), links.c_str(), &out));
    EXPECT_EQ(json::parse(take(out)).size(), 1u);
    EXPECT_TRUE(fs::exists(fs::path(links) / "a" / "b" / "copy.txt"));
}

// The restored meta rows name entries like the exports do
TEST(CAbiEntryPath, restoredMetaPathsAreNormalized) {
    const Dataset ds(TEST_NAME, "sub/a.txt");
    const auto row = [](const std::string& path, const std::string& id) {
        return json{{"id", id},
                    {"path", path},
                    {"key", "note"},
                    {"data", "\"restored\""},
                    {"mtime", 1700000000}};
    };

    char* out = nullptr;
    const std::string accepted = json::array({row("sub\\a.txt", "id-1")}).dump();
    ASSERT_DDB_OK(DDBMetaRestore(ds.path().c_str(), accepted.c_str(), &out));
    EXPECT_EQ(json::parse(take(out)).value("restored", 0), 1);
    ASSERT_DDB_OK(DDBMetaList(ds.path().c_str(), "sub/a.txt", &out));
    EXPECT_EQ(json::parse(take(out)).size(), 1u);

    // The dataset itself ("") and refused paths
    const std::string dataset = json::array({row("", "id-2")}).dump();
    ASSERT_DDB_OK(DDBMetaRestore(ds.path().c_str(), dataset.c_str(), &out));
    take(out);
    for (const char* bad : {"../x", ".ddb/x", "/abs"}) {
        const std::string refused = json::array({row(bad, "id-3")}).dump();
        EXPECT_EQ(DDBMetaRestore(ds.path().c_str(), refused.c_str(), &out), DDBERR_INVALIDARGS)
            << bad;
    }
    ASSERT_DDB_OK(DDBMetaList(ds.path().c_str(), "", &out));
    EXPECT_EQ(json::parse(take(out)).size(), 2u) << "the refused rows were not restored";
}

// The exports of filesystem paths keep their paths: an absolute path of a file being added, and
// the absolute path DDBRemove and DDBList get (they resolve it against the dataset root)
TEST(CAbiEntryPath, filesystemPathsAreNotEntryPaths) {
    Dataset ds(TEST_NAME, "sub/a.txt");
    const std::string absolute = (ds.root / "sub" / "a.txt").string();
    const char* paths[] = {absolute.c_str()};

    char* out = nullptr;
    ASSERT_DDB_OK(DDBList(ds.path().c_str(), paths, 1, &out, "json", false, 0));
    EXPECT_EQ(firstPath(take(out)), "sub/a.txt");
    ASSERT_DDB_OK(DDBInfo(paths, 1, &out, "json", false, 0, "auto", false, true));
    take(out);
    ASSERT_DDB_OK(DDBRemove(ds.path().c_str(), paths, 1));
    EXPECT_EQ(DDBGet(ds.path().c_str(), "sub/a.txt", &out), DDBERR_NOTFOUND);

    // The file outside the dataset: not an entry, nothing to remove
    const std::string outside = ds.area.getPath("outside.txt").string();
    std::ofstream(outside) << "x";
    const char* outsidePaths[] = {outside.c_str()};
    EXPECT_NE(DDBRemove(ds.path().c_str(), outsidePaths, 1), DDBERR_NONE);
    EXPECT_TRUE(fs::exists(outside));
}

}  // namespace
