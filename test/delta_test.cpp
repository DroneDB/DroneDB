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

    EXPECT_EQ(delta.copies.size(), 0);
    EXPECT_EQ(delta.adds.size(), 1);
    EXPECT_EQ(delta.removes.size(), 0);

    EXPECT_EQ(delta.adds[0].path, "4.jpg");
    EXPECT_EQ(delta.adds[0].type, Generic);
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

    EXPECT_EQ(delta.copies.size(), 0);
    EXPECT_EQ(delta.adds.size(), 0);
    EXPECT_EQ(delta.removes.size(), 1);

    EXPECT_EQ(delta.removes[0].path, "3.jpg");
    EXPECT_EQ(delta.removes[0].type, Generic);
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

    EXPECT_EQ(delta.copies.size(), 1);
    EXPECT_EQ(delta.adds.size(), 0);
    EXPECT_EQ(delta.removes.size(), 1);

    EXPECT_EQ(delta.copies[0].source, "3.jpg");
    EXPECT_EQ(delta.copies[0].destination, "3-new.jpg");

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

    EXPECT_EQ(delta.copies.size(), 0);
    EXPECT_EQ(delta.adds.size(), 0);
    EXPECT_EQ(delta.removes.size(), 0);
   
}

TEST(deltaList, complexTree1) {

    const std::vector<SimpleEntry> dest{
        SimpleEntry("1.jpg", "AAA"),
        SimpleEntry("2.jpg", "BBB"),
        SimpleEntry("3.jpg", "CCC"),
        SimpleEntry("img"),
        SimpleEntry("img/1.jpg", "AAA"),
        SimpleEntry("img/2.jpg", "BBB"),
        SimpleEntry("img/3.jpg", "CCC")};

    const std::vector<SimpleEntry> source{
        SimpleEntry("1.jpg", "CCC"),
        SimpleEntry("2.jpg", "AAA"),
        SimpleEntry("3.jpg", "BBB"),
        SimpleEntry("cov"),
        SimpleEntry("cov/1.jpg", "BBB"),
        SimpleEntry("cov/2.jpg", "CCC"),
        SimpleEntry("cov/3.jpg", "AAA")};

    const auto delta = getDelta(source, dest);

    const auto expected = R"(
    {
  "adds": [
    {
      "path": "cov",
      "type": 1
    }
  ],
  "copies": [
    ["3.jpg", "1.jpg"
    ],
    ["1.jpg", "2.jpg"
    ],
    ["2.jpg", "3.jpg"
    ],
    ["2.jpg", "cov/1.jpg"
    ],
    ["3.jpg", "cov/2.jpg"
    ],
    ["1.jpg", "cov/3.jpg"
    ]
  ],
  "removes": [
    {
      "path": "img/3.jpg",
      "type": 2
    },
    {
      "path": "img/2.jpg",
      "type": 2
    },
    {
      "path": "img/1.jpg",
      "type": 2
    },
    {
      "path": "img",
      "type": 1
    }
  ]
}
    )"_json;

    const json j = delta;
    
    EXPECT_EQ(j, expected);
   
}

}  // namespace
