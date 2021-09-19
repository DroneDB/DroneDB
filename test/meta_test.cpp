
#include "gtest/gtest.h"
#include "dbops.h"
#include "exceptions.h"
#include "test.h"
#include "testarea.h"

namespace {

using namespace ddb;

TEST(meta, happyPath) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/"
        "dbase.sqlite",
        "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb",
             fs::copy_options::overwrite_existing);

    const auto tf = testFolder.string();

    const auto db = ddb::open(tf, false);

    MetaManager manager(db.get());

    // Should be nothing
    auto lst = manager.list();
    EXPECT_EQ(lst.size(), 0);

    // Add 3 annotations
    manager.add("annotations", "this is a string", "", tf);
    const auto itm =
        manager.add("annotations", R"({"test":"this is an object"})", "", tf);
    manager.add("annotations", R"({"test":1234, "dummy": [123,43,45,{}]})", "",
                tf);

    // Should be 3
    lst = manager.list();
    LOGD << lst.dump();
    EXPECT_EQ(lst.size(), 1);
    EXPECT_EQ(lst[0]["count"], 3);

    // Check data
    const auto gt = manager.get("annotations", "", tf);
    EXPECT_EQ(gt.size(), 3);
    EXPECT_EQ(gt[0]["data"], "this is a string");
    EXPECT_EQ(gt[1]["data"], json::parse(R"({"test":"this is an object"})"));
    EXPECT_EQ(gt[2]["data"],
              json::parse(R"({"test":1234, "dummy": [123,43,45,{}]})"));

    // Wrong key should throw exception
    EXPECT_THROW(manager.get("annotation", "", tf), InvalidArgsException);

    // Remove one annotation
    auto rm = manager.remove(itm["id"]);
    EXPECT_EQ(rm["removed"], 1);

    // Second time should do nothing
    rm = manager.remove(itm["id"]);
    EXPECT_EQ(rm["removed"], 0);

    // Set config meta
    auto cfg = manager.set("config", R"([123,432,"ehy"])", "", tf);
    LOGD << cfg.dump();

    // Check config data
    EXPECT_EQ(cfg["data"], json::parse(R"([123,432,"ehy"])"));

    // Check list count
    lst = manager.list();
    EXPECT_EQ(lst.size(), 2);
    EXPECT_EQ(lst[0]["count"], 2);
    EXPECT_EQ(lst[1]["count"], 1);

    // Unset annotations (2 left)
    auto us = manager.unset("annotations", "", tf);
    EXPECT_EQ(us["removed"], 2);

    // Check list count (1)
    lst = manager.list();
    EXPECT_EQ(lst.size(), 1);
    EXPECT_EQ(lst[0]["count"], 1);

    // Remove config
    rm = manager.remove(cfg["id"]);
    EXPECT_EQ(rm["removed"], 1);

    // Check list count (0)
    lst = manager.list();
    EXPECT_EQ(lst.size(), 0);
}

TEST(meta, happyPathWithPath) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/"
        "dbase.sqlite",
        "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb",
             fs::copy_options::overwrite_existing);

    const auto tf = testFolder.string();

    LOGD << "TestFolder = " << tf;

    const auto db = ddb::open(tf, false);

    MetaManager manager(db.get());

    // Should be nothing
    auto lst = manager.list("", tf);
    EXPECT_EQ(lst.size(), 0);

    // Add 3 annotations
    manager.add("annotations", "this is a string", "1JI_0065.JPG", tf);
    const auto itm = manager.add("annotations", R"({"test":"this is an object"})", "1JI_0065.JPG", tf);
    manager.add("annotations", R"({"test":1234, "dummy": [123,43,45,{}]})", "1JI_0064.JPG", tf);

    // Should be 0 without path
    lst = manager.list("", tf);
    LOGD << lst.dump();
    EXPECT_EQ(lst.size(), 2);
    EXPECT_EQ(lst[0]["count"], 1);
    EXPECT_EQ(lst[1]["count"], 2);

    // Should be 2 with path "1JI_0065.JPG"
    lst = manager.list("1JI_0065.JPG", tf);
    LOGD << lst.dump();
    EXPECT_EQ(lst.size(), 1);
    EXPECT_EQ(lst[0]["count"], 2);

    // Should be 1 with path "1JI_0064.JPG"
    lst = manager.list("1JI_0064.JPG", tf);
    LOGD << lst.dump();
    EXPECT_EQ(lst.size(), 1);
    EXPECT_EQ(lst[0]["count"], 1);

    // Unset annotations with path "1JI_0065.JPG" (2 left)
    auto us = manager.unset("annotations", "1JI_0065.JPG", tf);
    EXPECT_EQ(us["removed"], 2);

    // Unset annotations with path "1JI_0064.JPG" (2 left)
    us = manager.unset("annotations", "1JI_0064.JPG", tf);
    EXPECT_EQ(us["removed"], 1);

    // Check list count (0)
    lst = manager.list("", tf);
    EXPECT_EQ(lst.size(), 0);
}

TEST(meta, variousErrors) {
    TestArea ta(TEST_NAME);

    const auto sqlite = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/ddb-remove-test/.ddb/"
        "dbase.sqlite",
        "dbase.sqlite");

    const auto testFolder = ta.getFolder("test");
    create_directory(testFolder / ".ddb");
    fs::copy(sqlite.string(), testFolder / ".ddb",
             fs::copy_options::overwrite_existing);

    const auto tf = testFolder.string();

    const auto db = ddb::open(tf, false);

    MetaManager manager(db.get());

    // Path does not exist
    EXPECT_THROW(manager.list("WOUYIFBGHOPWU", tf), InvalidArgsException);

    // Malformed JSON
    EXPECT_THROW(manager.add("annotations", "{\"ciao\":}", "", tf), JSONException);

    // Expect plural
    EXPECT_THROW(manager.add("annotation", "1234", "", tf), InvalidArgsException);
    
}



} 
