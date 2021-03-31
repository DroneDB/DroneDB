/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dbops.h"
#include "delta.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "registry.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;

TEST(utilsTest, generateRandomString) {
    for (auto n = 0; n < 1000; n++) {
        auto str = utils::generateRandomString(100);
        // Uncomment for the WALL OF RANDOM
        // std::cout << str << std::endl;
        EXPECT_TRUE(str.length() == 100);
    }
}

void performDeltaTest(const std::vector<SimpleEntry> dest,
                      const std::vector<SimpleEntry> source) {
    bool equal = false;

    fs::path sourceFolder;
    fs::path destFolder;

    try {
        sourceFolder = makeTree(source);
        destFolder = makeTree(dest);

        std::cout << "SourceTree = " << sourceFolder << std::endl;
        std::cout << "DestTree = " << destFolder << std::endl;

        auto res = getDelta(source, dest);

        const json j = res;

        std::cout << std::endl << "Delta:" << std::endl;
        std::cout << j.dump(4);

        applyDelta(res, destFolder, sourceFolder);

        equal = compareTree(sourceFolder, destFolder);

        std::cout << "Equal? " << equal << std::endl;
    } catch (std::runtime_error& e) {
        std::cout << "Exception: " << e.what() << std::endl;
    }

    if (!sourceFolder.empty() && exists(sourceFolder)) remove_all(sourceFolder);
    if (!destFolder.empty() && exists(destFolder)) remove_all(destFolder);

    EXPECT_TRUE(equal);
}

TEST(applyDeltaTest, simpleAdd) {
    const std::vector<SimpleEntry> dest{
        SimpleEntry("a"),
        SimpleEntry("a/.ddb"),
        SimpleEntry("a/.ddb/dbase.sqlite", "BBB"),
        SimpleEntry("a/a.txt", "AAA"),
        SimpleEntry("a/b"),
        SimpleEntry("a/b/c.txt", "AAA")};

    const std::vector<SimpleEntry> source{
        SimpleEntry("1.txt", "AAA"), SimpleEntry("2.txt", "BBB"),
        SimpleEntry("3.txt", "CCC"), SimpleEntry("4.txt", "DDD")};

    performDeltaTest(dest, source);
}

TEST(applyDeltaTest, simpleRename) {
    const std::vector<SimpleEntry> dest = {SimpleEntry("1.jpg", "AAA"),
                                           SimpleEntry("2.jpg", "BBB"),
                                           SimpleEntry("5.jpg", "GGG")};

    const std::vector<SimpleEntry> source = {
        SimpleEntry("1.jpg", "EEE"), SimpleEntry("2.jpg", "FFF"),
        SimpleEntry("3.jpg", "AAA"), SimpleEntry("4.jpg", "BBB")};

    performDeltaTest(dest, source);
}

TEST(applyDeltaTest, complexTree2) {
    const std::vector<SimpleEntry> dest = {
        SimpleEntry("ciao.txt", "CIAO"), SimpleEntry("pippo.txt", "PIPPO"),
        SimpleEntry("test"), SimpleEntry("test/a.txt", "AAA"),
        SimpleEntry("test/b.txt", "BBB")};

    const std::vector<SimpleEntry> source = {SimpleEntry("lol.txt", "COPIA"),
                                             SimpleEntry("plutone.txt", "CIAO"),
                                             SimpleEntry("pippo.txt", "PIPPO"),
                                             SimpleEntry("tast"),
                                             SimpleEntry("tast/a.txt", "AAA"),
                                             SimpleEntry("tast/b.txt", "BBB"),
                                             SimpleEntry("tast/c.txt", "AAA"),
                                             SimpleEntry("tast/d.txt", "DDD"),
                                             SimpleEntry("test"),
                                             SimpleEntry("test/a.txt", "AAA"),
                                             SimpleEntry("test/b.txt", "BBB")};

    performDeltaTest(dest, source);
}

}  // namespace
