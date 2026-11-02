/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The Registry hashes an upload while it streams to disk and passes the digest to
// DDBAddWithHints. A hint is used only while the file keeps the size and mtime it had when the
// hash was computed, so a stale hint never indexes a wrong hash.

#include "ddb.h"
#include "dbops.h"
#include "gtest/gtest.h"
#include "hash.h"
#include "json.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;

const char *const FakeHash = "0000000000000000000000000000000000000000000000000000000000000000";

struct HintedFile {
    fs::path path;
    long long size;
    double mtime;
};

fs::path initDataset(TestArea &ta) {
    const auto root = ta.getFolder("ds");
    char *outPath = nullptr;
    EXPECT_EQ(DDBInit(root.string().c_str(), &outPath), DDBERR_NONE);
    DDBFree(outPath);
    return root;
}

// Writes the file and returns what a caller would hint for it
HintedFile writeHintedFile(const fs::path &path, const std::string &content) {
    fileWriteAllText(path, content);
    return {path, static_cast<long long>(fs::file_size(path)), io::Path(path).getModifiedTimePrecise()};
}

json addWithHint(const fs::path &root, const HintedFile &f, const char *sha256, bool stopOnError = true) {
    const std::string path = f.path.string();
    DDBAddHint hint{path.c_str(), sha256, f.size, f.mtime};
    DDBAddOptions opts{false, stopOnError, 2};

    char *out = nullptr;
    EXPECT_EQ(DDBAddWithHints(root.string().c_str(), &hint, 1, &opts, &out), DDBERR_NONE) << DDBGetLastError();
    auto j = json::parse(out);
    DDBFree(out);
    return j;
}

TEST(DDBAddWithHints, matchingHintIsUsedWithoutReadingTheFile) {
    TestArea ta(TEST_NAME, true);
    const auto root = initDataset(ta);
    const auto f = writeHintedFile(root / "a.bin", "the content");

    // The fake digest can only come from the hint: the file was not read
    const auto j = addWithHint(root, f, FakeHash);

    ASSERT_EQ(j["entries"].size(), 1u);
    EXPECT_EQ(j["entries"][0]["hash"].get<std::string>(), FakeHash);
    EXPECT_EQ(j["entries"][0]["size"].get<long long>(), f.size);
}

TEST(DDBAddWithHints, sizeMismatchHashesTheFile) {
    TestArea ta(TEST_NAME, true);
    const auto root = initDataset(ta);
    auto f = writeHintedFile(root / "a.bin", "the content");
    f.size += 1; // the writer saw another size: the hint is stale

    const auto j = addWithHint(root, f, FakeHash);

    ASSERT_EQ(j["entries"].size(), 1u);
    EXPECT_EQ(j["entries"][0]["hash"].get<std::string>(), Hash::fileSHA256((root / "a.bin").string()));
}

TEST(DDBAddWithHints, mtimeMismatchHashesTheFile) {
    TestArea ta(TEST_NAME, true);
    const auto root = initDataset(ta);
    auto f = writeHintedFile(root / "a.bin", "the content");
    f.mtime -= 10; // written again after the hash was computed

    const auto j = addWithHint(root, f, FakeHash);

    ASSERT_EQ(j["entries"].size(), 1u);
    EXPECT_EQ(j["entries"][0]["hash"].get<std::string>(), Hash::fileSHA256((root / "a.bin").string()));
}

TEST(DDBAddWithHints, noDigestHashesTheFile) {
    TestArea ta(TEST_NAME, true);
    const auto root = initDataset(ta);
    const auto f = writeHintedFile(root / "a.bin", "the content");

    const auto j = addWithHint(root, f, nullptr);

    ASSERT_EQ(j["entries"].size(), 1u);
    EXPECT_EQ(j["entries"][0]["hash"].get<std::string>(), Hash::fileSHA256((root / "a.bin").string()));
}

TEST(DDBAddWithHints, replacedFileWithNewHintIsUpdated) {
    TestArea ta(TEST_NAME, true);
    const auto root = initDataset(ta);
    const auto first = writeHintedFile(root / "a.bin", "first");
    const auto firstHash = Hash::fileSHA256(first.path.string());
    ASSERT_EQ(addWithHint(root, first, firstHash.c_str())["entries"].size(), 1u);

    const auto second = writeHintedFile(root / "a.bin", "second, longer");
    const auto secondHash = Hash::fileSHA256(second.path.string());
    const auto j = addWithHint(root, second, secondHash.c_str());

    ASSERT_EQ(j["entries"].size(), 1u);
    EXPECT_EQ(j["entries"][0]["status"].get<std::string>(), "updated");
    EXPECT_EQ(j["entries"][0]["hash"].get<std::string>(), secondHash);
}

TEST(DDBAddWithHints, missingFileFailsOnlyItselfWithoutStopOnError) {
    TestArea ta(TEST_NAME, true);
    const auto root = initDataset(ta);
    const auto good = writeHintedFile(root / "good.bin", "good");
    const std::string goodPath = good.path.string();
    const std::string gonePath = (root / "gone.bin").string();

    const DDBAddHint hints[] = {{goodPath.c_str(), nullptr, good.size, good.mtime},
                                {gonePath.c_str(), FakeHash, 4, 0}};
    DDBAddOptions opts{false, false, 2};
    char *out = nullptr;
    ASSERT_EQ(DDBAddWithHints(root.string().c_str(), hints, 2, &opts, &out), DDBERR_NONE);
    const auto j = json::parse(out);
    DDBFree(out);

    EXPECT_EQ(j["entries"].size(), 1u);
    ASSERT_EQ(j["errors"].size(), 1u);
    EXPECT_EQ(j["errors"][0]["path"].get<std::string>(), "gone.bin");
}

TEST(DDBAddWithHints, missingFileAbortsWithStopOnError) {
    TestArea ta(TEST_NAME, true);
    const auto root = initDataset(ta);
    const std::string gonePath = (root / "gone.bin").string();
    const DDBAddHint hint{gonePath.c_str(), FakeHash, 4, 0};
    DDBAddOptions opts{false, true, 2};

    char *out = nullptr;
    EXPECT_NE(DDBAddWithHints(root.string().c_str(), &hint, 1, &opts, &out), DDBERR_NONE);
}

} // namespace
