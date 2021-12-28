/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */


#include "dbops.h"
#include "delta.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

namespace {

using namespace ddb;

TEST(deltaList, simpleAdd) {
    const std::vector<SimpleEntry> dest{SimpleEntry("1.jpg", "AAA"),
                                        SimpleEntry("2.jpg", "BBB"),
                                        SimpleEntry("3.jpg", "CCC")};

    const std::vector<SimpleEntry> source{SimpleEntry("1.jpg", "AAA"),
                                          SimpleEntry("2.jpg", "BBB"),
                                          SimpleEntry("3.jpg", "CCC"),
                                          SimpleEntry("4.jpg", "DDD")};

    const auto delta = getDelta(source, dest);

    EXPECT_EQ(delta.adds.size(), 1);
    EXPECT_EQ(delta.removes.size(), 0);

    EXPECT_EQ(delta.adds[0].path, "4.jpg");
}

TEST(deltaList, simpleRemove) {
    const std::vector<SimpleEntry> dest{SimpleEntry("1.jpg", "AAA"),
                                        SimpleEntry("2.jpg", "BBB"),
                                        SimpleEntry("3.jpg", "CCC")};

    const std::vector<SimpleEntry> source{
        SimpleEntry("1.jpg", "AAA"),
        SimpleEntry("2.jpg", "BBB"),
    };

    const auto delta = getDelta(source, dest);

    EXPECT_EQ(delta.adds.size(), 0);
    EXPECT_EQ(delta.removes.size(), 1);

    EXPECT_EQ(delta.removes[0].path, "3.jpg");
}

TEST(deltaList, simpleCopy) {
    const std::vector<SimpleEntry> dest{SimpleEntry("1.jpg", "AAA"),
                                        SimpleEntry("2.jpg", "BBB"),
                                        SimpleEntry("3.jpg", "CCC")};

    const std::vector<SimpleEntry> source{
        SimpleEntry("1.jpg", "AAA"),
        SimpleEntry("2.jpg", "BBB"),
        SimpleEntry("3-new.jpg", "CCC"),
    };

    const auto delta = getDelta(source, dest);

    EXPECT_EQ(delta.adds.size(), 0);
    EXPECT_EQ(delta.removes.size(), 1);

    EXPECT_EQ(delta.removes[0].path, "3.jpg");
}

TEST(deltaList, edgeCase1) {

    const std::vector<SimpleEntry> dest{
        SimpleEntry("a"),
        SimpleEntry("a/.ddb"),
        SimpleEntry("a/.ddb/dbase.sqlite", "BBB"),
        SimpleEntry("a/a.txt", "AAA"),
        SimpleEntry("a/b"),
        SimpleEntry("a/b/c.txt", "AAA")};

    const std::vector<SimpleEntry> source{
        SimpleEntry("a"),
        SimpleEntry("a/.ddb"),
        SimpleEntry("a/.ddb/dbase.sqlite", "BBB"),
        SimpleEntry("a/a.txt", "AAA"),
        SimpleEntry("a/b"),
        SimpleEntry("a/b/c.txt", "AAA")};

    const auto delta = getDelta(source, dest);

    EXPECT_EQ(delta.adds.size(), 0);
    EXPECT_EQ(delta.removes.size(), 0);
   
}

}  // namespace
