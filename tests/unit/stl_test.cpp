/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// STL files as Models: how identifyStl classifies a binary or an ASCII file (an STL has no magic
// number, so a name like "solid" proves nothing), what indexing and rescanning make of an .stl,
// and what Assimp reads from one.

#include "stl.h"

#include <string>

#include "3d.h"
#include "dbops.h"
#include "entry.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;

// A binary STL of n triangles whose count field says `declared`
std::string binaryStlWithCount(uint32_t declared, size_t n) {
    std::string out(80, ' ');
    out.append(reinterpret_cast<const char*>(&declared), sizeof(declared));
    out.append(n * 50, '\0');
    return out;
}

TEST(stl, binaryAndAsciiGridsAreModels) {
    TestArea ta(TEST_NAME, true);
    EXPECT_EQ(identifyStl(writeFile(ta.getPath("ascii.stl"), gridStl(5, false))), EntryType::Model);
    EXPECT_EQ(identifyStl(writeFile(ta.getPath("binary.stl"), gridStl(5, true))), EntryType::Model);
}

// A binary file whose 80-byte header starts with "solid" is still binary: the size decides
TEST(stl, binaryWithSolidHeaderIsAModel) {
    TestArea ta(TEST_NAME, true);
    EXPECT_EQ(identifyStl(writeFile(ta.getPath("solid_header.stl"), gridStl(5, true, true))),
              EntryType::Model);
}

TEST(stl, asciiVariantsAreModels) {
    TestArea ta(TEST_NAME, true);
    const std::string facet =
        "facet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 "
        "0\nendloop\nendfacet\n";
    const auto model = [&](const std::string& name, const std::string& content) {
        EXPECT_EQ(identifyStl(writeFile(ta.getPath(name), content)), EntryType::Model) << name;
    };

    model("crlf.stl", "solid part\r\nfacet normal 0 0 1\r\nouter loop\r\nvertex 0 0 0\r\n");
    model("upper.stl", "SOLID PART\nFACET NORMAL 0 0 1\nOUTER LOOP\n");
    model("noname.stl", "solid\n" + facet);
    model("bom_and_blank_lines.stl", "\xEF\xBB\xBF\n  solid part\n\n   " + facet);
    // The name of the solid is free text, whatever it spells
    model("facet_in_name.stl", "solid facet normal\n" + facet);
}

TEST(stl, filesThatAreNotATriangleMeshAreGeneric) {
    TestArea ta(TEST_NAME, true);
    const auto generic = [&](const std::string& name, const std::string& content) {
        EXPECT_EQ(identifyStl(writeFile(ta.getPath(name), content)), EntryType::Generic) << name;
    };

    generic("empty.stl", "");
    generic("garbage.stl", "this is not a mesh, whatever the extension says\n");
    generic("random.stl", std::string(5000, '\x07'));
    generic("solid_only.stl", "solid part\n");
    generic("no_facets.stl", "solid part\nendsolid part\n");
    generic("no_line_break.stl", "solid part facet normal 0 0 1");
    generic("prefix_of_a_word.stl", "solidity\nfacet normal 0 0 1\n");
    generic("facet_without_normal.stl", "solid part\nfacet 0 0 1\n");

    // Binary: a count of zero, a count the size does not match, a cut file, a header only
    generic("zero_triangles.stl", binaryStlWithCount(0, 0));
    generic("count_too_high.stl", binaryStlWithCount(3, 2));
    generic("count_too_low.stl", binaryStlWithCount(1, 2));
    generic("huge_count.stl", binaryStlWithCount(0xFFFFFFFFu, 2));
    generic("header_only.stl", std::string(80, ' '));
    std::string cut = gridStl(5, true);
    cut.resize(cut.size() - 10);
    generic("truncated.stl", cut);
    // A binary file cut so that it starts like an ASCII one is no ASCII file either
    std::string solidCut = gridStl(5, true, true);
    solidCut.resize(solidCut.size() - 1);
    generic("solid_header_truncated.stl", solidCut);
}

TEST(stl, missingFileIsGeneric) {
    TestArea ta(TEST_NAME, true);
    EXPECT_EQ(identifyStl(ta.getPath("nothing.stl")), EntryType::Generic);
}

TEST(stl, fingerprintFollowsTheContentNotTheName) {
    TestArea ta(TEST_NAME, true);
    EXPECT_EQ(fingerprint(writeFile(ta.getPath("a.STL"), gridStl(3, true))), EntryType::Model);
    EXPECT_EQ(fingerprint(writeFile(ta.getPath("b.stl"), gridStl(3, false))), EntryType::Model);
    EXPECT_EQ(fingerprint(writeFile(ta.getPath("c.stl"), "not a mesh")), EntryType::Generic);
}

TEST(stl, indexedAsModelOrGeneric) {
    IndexFixture fx(TEST_NAME);
    for (const auto& [name, content] :
         {std::pair<std::string, std::string>{"ascii.stl", gridStl(4, false)},
          {"binary.stl", gridStl(4, true)},
          {"garbage.stl", "garbage"}})
        writeFile(fx.root / name, content);

    addToIndex(fx.db.get(),
               {(fx.root / "ascii.stl").string(),
                (fx.root / "binary.stl").string(),
                (fx.root / "garbage.stl").string()});

    Entry e;
    ASSERT_TRUE(loadEntry(fx.db.get(), "ascii.stl", e));
    EXPECT_EQ(e.type, EntryType::Model);
    ASSERT_TRUE(loadEntry(fx.db.get(), "binary.stl", e));
    EXPECT_EQ(e.type, EntryType::Model);
    ASSERT_TRUE(loadEntry(fx.db.get(), "garbage.stl", e));
    EXPECT_EQ(e.type, EntryType::Generic);
}

// An .stl indexed before STL was a Model stays Generic until it is rescanned: no migration, the
// existing rescan re-detects it
TEST(stl, rescanTurnsAnOldGenericEntryIntoAModel) {
    IndexFixture fx(TEST_NAME);
    writeFile(fx.root / "old.stl", gridStl(4, true));
    writeFile(fx.root / "other.stl", "garbage");
    addToIndex(fx.db.get(), {(fx.root / "old.stl").string(), (fx.root / "other.stl").string()});

    // What an index written by an earlier version holds
    auto update = fx.db->query("UPDATE entries SET type = ? WHERE path = 'old.stl'");
    update->bind(1, static_cast<int>(EntryType::Generic));
    update->execute();
    Entry e;
    ASSERT_TRUE(loadEntry(fx.db.get(), "old.stl", e));
    ASSERT_EQ(e.type, EntryType::Generic);

    rescanIndex(fx.db.get(), {EntryType::Generic}, {"stl"}, true, nullptr);

    ASSERT_TRUE(loadEntry(fx.db.get(), "old.stl", e));
    EXPECT_EQ(e.type, EntryType::Model);
    ASSERT_TRUE(loadEntry(fx.db.get(), "other.stl", e));
    EXPECT_EQ(e.type, EntryType::Generic);
}

// Assimp (getModelInfo) must have its STL importer: the bounds and the triangle count of both
// flavours
TEST(stl, modelInfoReadsBinaryAndAscii) {
    TestArea ta(TEST_NAME, true);
    for (const bool binary : {true, false}) {
        const fs::path stl = writeFile(ta.getPath(binary ? "b.stl" : "a.stl"), gridStl(5, binary));
        ModelInfo info;
        ASSERT_TRUE(getModelInfo(stl.string(), info)) << (binary ? "binary" : "ascii");
        EXPECT_TRUE(info.hasBounds);
        EXPECT_EQ(info.faceCount, 32u);
        EXPECT_FLOAT_EQ(static_cast<float>(info.minX), 0.f);
        EXPECT_FLOAT_EQ(static_cast<float>(info.maxX), 4.f);
        EXPECT_FLOAT_EQ(static_cast<float>(info.minY), 0.f);
        EXPECT_FLOAT_EQ(static_cast<float>(info.maxY), 4.f);
        EXPECT_FLOAT_EQ(static_cast<float>(info.maxZ), 0.5f);
    }

    ModelInfo none;
    EXPECT_FALSE(getModelInfo(writeFile(ta.getPath("garbage.stl"), "garbage").string(), none));
}

}  // namespace
