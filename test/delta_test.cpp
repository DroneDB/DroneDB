/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <delta.h>

#include "dbops.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

namespace {

using namespace ddb;

TEST(deltaList, simpleAdd) {
    const std::vector<SimpleEntry> dest{SimpleEntry("1.jpg", "AAA", Generic),
                                        SimpleEntry("2.jpg", "BBB", Generic),
                                        SimpleEntry("3.jpg", "CCC", Generic)};

    const std::vector<SimpleEntry> source{SimpleEntry("1.jpg", "AAA", Generic),
                                          SimpleEntry("2.jpg", "BBB", Generic),
                                          SimpleEntry("3.jpg", "CCC", Generic),
                                          SimpleEntry("4.jpg", "DDD", Generic)};

    const auto delta = getDelta(source, dest);

    EXPECT_EQ(delta.copies.size(), 0);
    EXPECT_EQ(delta.adds.size(), 1);
    EXPECT_EQ(delta.removes.size(), 0);

    EXPECT_EQ(delta.adds[0].path, "4.jpg");
    EXPECT_EQ(delta.adds[0].type, Generic);
}

TEST(deltaList, simpleRemove) {
    const std::vector<SimpleEntry> dest{SimpleEntry("1.jpg", "AAA", Generic),
                                        SimpleEntry("2.jpg", "BBB", Generic),
                                        SimpleEntry("3.jpg", "CCC", Generic)};

    const std::vector<SimpleEntry> source{
        SimpleEntry("1.jpg", "AAA", Generic),
        SimpleEntry("2.jpg", "BBB", Generic),
    };

    const auto delta = getDelta(source, dest);

    EXPECT_EQ(delta.copies.size(), 0);
    EXPECT_EQ(delta.adds.size(), 0);
    EXPECT_EQ(delta.removes.size(), 1);

    EXPECT_EQ(delta.removes[0].path, "3.jpg");
    EXPECT_EQ(delta.removes[0].type, Generic);
}

TEST(deltaList, simpleCopy) {
    const std::vector<SimpleEntry> dest{SimpleEntry("1.jpg", "AAA", Generic),
                                        SimpleEntry("2.jpg", "BBB", Generic),
                                        SimpleEntry("3.jpg", "CCC", Generic)};

    const std::vector<SimpleEntry> source{
        SimpleEntry("1.jpg", "AAA", Generic),
        SimpleEntry("2.jpg", "BBB", Generic),
        SimpleEntry("3-new.jpg", "CCC", Generic),
    };

    const auto delta = getDelta(source, dest);

    EXPECT_EQ(delta.copies.size(), 1);
    EXPECT_EQ(delta.adds.size(), 0);
    EXPECT_EQ(delta.removes.size(), 1);

    EXPECT_EQ(delta.copies[0].source, "3.jpg");
    EXPECT_EQ(delta.copies[0].destination, "3-new.jpg");

    EXPECT_EQ(delta.removes[0].path, "3.jpg");
}

TEST(deltaList, edgeCase1) {

    const std::vector<SimpleEntry> dest{
        SimpleEntry("a", "", Directory),
        SimpleEntry("a/.ddb", "", Directory),
        SimpleEntry("a/.ddb/dbase.sqlite", "BBB", Generic),
        SimpleEntry("a/a.txt", "AAA", Generic),
        SimpleEntry("a/b", "", Directory),
        SimpleEntry("a/b/c.txt", "AAA", Generic)};

    const std::vector<SimpleEntry> source{
        SimpleEntry("a", "", Directory),
        SimpleEntry("a/.ddb", "", Directory),
        SimpleEntry("a/.ddb/dbase.sqlite", "BBB", Generic),
        SimpleEntry("a/a.txt", "AAA", Generic),
        SimpleEntry("a/b", "", Directory),
        SimpleEntry("a/b/c.txt", "AAA", Generic)};

    const auto delta = getDelta(source, dest);

    EXPECT_EQ(delta.copies.size(), 0);
    EXPECT_EQ(delta.adds.size(), 0);
    EXPECT_EQ(delta.removes.size(), 0);
   
}

}  // namespace
