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

TEST(applyDeltaTest, hardRename) {
    const std::vector<SimpleEntry> dest = {
        SimpleEntry("1.jpg", "AAA"),     SimpleEntry("2.jpg", "BBB"),
        SimpleEntry("3.jpg", "CCC"),     SimpleEntry("img"),
        SimpleEntry("img/1.jpg", "AAA"), SimpleEntry("img/2.jpg", "BBB"),
        SimpleEntry("img/3.jpg", "CCC")};

    const std::vector<SimpleEntry> source = {
        SimpleEntry("1.jpg", "CCC"),     SimpleEntry("2.jpg", "AAA"),
        SimpleEntry("3.jpg", "BBB"),     SimpleEntry("cov"),
        SimpleEntry("cov/1.jpg", "BBB"), SimpleEntry("cov/2.jpg", "CCC"),
        SimpleEntry("cov/3.jpg", "AAA")};

    performDeltaTest(dest, source);
}

TEST(applyDeltaTest, hardRename2) {
    const std::vector<SimpleEntry> dest = {
        SimpleEntry("1.jpg", "AAA"),     SimpleEntry("2.jpg", "BBB"),
        SimpleEntry("3.jpg", "CCC"),     SimpleEntry("img"),
        SimpleEntry("img/1.jpg", "DDD"), SimpleEntry("img/2.jpg", "EEE"),
        SimpleEntry("img/3.jpg", "FFF")};

    const std::vector<SimpleEntry> source = {SimpleEntry("1.jpg", "BBB"),
                                             SimpleEntry("2.jpg", "BBB"),
                                             SimpleEntry("3.jpg", "BBB"),
                                             SimpleEntry("4.jpg", "AAA"),
                                             SimpleEntry("5.jpg", "AAA"),
                                             SimpleEntry("6.jpg", "CCC"),
                                             SimpleEntry("cov"),
                                             SimpleEntry("cov/1.jpg", "AAA"),
                                             SimpleEntry("cov/2.jpg", "AAA"),
                                             SimpleEntry("cov/3.jpg", "AAA")};

    performDeltaTest(dest, source);
}

TEST(applyDeltaTest, hardRename3) {
    const std::vector<SimpleEntry> dest = {
        SimpleEntry("1.jpg", "AAA"),
        SimpleEntry("2.jpg", "BBB"),
        SimpleEntry("3.jpg", "CCC"),
        SimpleEntry("cov"),
        SimpleEntry("cov/cov"),
        SimpleEntry("cov/cov/cov"),
        SimpleEntry("cov/cov/covie.jpg", "ZZZ"),

    };

    const std::vector<SimpleEntry> source = {
        SimpleEntry("1.jpg", "CCC"),     SimpleEntry("2.jpg", "AAA"),
        SimpleEntry("3.jpg", "BBB"),     SimpleEntry("cov"),
        SimpleEntry("cov/1.jpg", "BBB"), SimpleEntry("cov/2.jpg", "CCC"),
        SimpleEntry("cov/3.jpg", "AAA")};

    performDeltaTest(dest, source);
}

TEST(applyDeltaTest, hardRename4) {
    const std::vector<SimpleEntry> dest = {
        SimpleEntry("img"),
        SimpleEntry("img/1.jpg", "AAA"),
        SimpleEntry("img/2.jpg", "BBB"),
        SimpleEntry("img/3.jpg", "CCC"),
        SimpleEntry("cov"),
        SimpleEntry("cov/cov"),
        SimpleEntry("cov/cov/cov"),
        SimpleEntry("cov/cov/covie.jpg", "ZZZ"),

    };

    const std::vector<SimpleEntry> source = {SimpleEntry("cov"),
                                             SimpleEntry("cov/1.jpg", "BBB"),
                                             SimpleEntry("cov/2.jpg", "CCC"),
                                             SimpleEntry("cov/3.jpg", "AAA"),
                                             SimpleEntry("pic"),
                                             SimpleEntry("pic/1.jpg", "CCC"),
                                             SimpleEntry("pic/2.jpg", "AAA"),
                                             SimpleEntry("pic/3.jpg", "BBB")};

    performDeltaTest(dest, source);
}

TEST(applyDeltaTest, hardRename5) {
    const std::vector<SimpleEntry> dest = {
        SimpleEntry("1.jpg", "AAA"), SimpleEntry("2.jpg", "BBB"),
        SimpleEntry("3.jpg", "CCC"), SimpleEntry("4.jpg", "DDD"),
        SimpleEntry("5.jpg", "EEE"), SimpleEntry("6.jpg", "FFF"),
    };

    const std::vector<SimpleEntry> source = {
        SimpleEntry("1.jpg", "FFF"),
        SimpleEntry("2.jpg", "AAA"),
        SimpleEntry("3.jpg", "BBB"),
        SimpleEntry("4.jpg", "CCC"),
        SimpleEntry("5.jpg", "DDD"),
        SimpleEntry("6.jpg", "EEE"),
        SimpleEntry("pics"),
        SimpleEntry("pics/1.jpg", "AAA"),
        SimpleEntry("pics/2.jpg", "BBB"),
        SimpleEntry("pics/3.jpg", "CCC"),
        SimpleEntry("pics/4.jpg", "DDD"),
        SimpleEntry("pics/5.jpg", "EEE"),
        SimpleEntry("pics/6.jpg", "FFF"),

    };

    performDeltaTest(dest, source);
}

TEST(applyDeltaTest, deepTree2) {
    const std::vector<SimpleEntry> dest = {
        SimpleEntry("1.jpg", "AAA"),
        SimpleEntry("pics"),
        SimpleEntry("pics/1.jpg", "AAA"),
        SimpleEntry("pics/pics"),
        SimpleEntry("pics/pics/1.jpg", "AAA"),
        SimpleEntry("pics/pics/pics"),
        SimpleEntry("pics/pics/pics/1.jpg", "AAA"),
        SimpleEntry("2.jpg", "EEE"),
        SimpleEntry("pics2"),
        SimpleEntry("pics2/3.jpg", "GGG"),
        SimpleEntry("pics2/pics2"),
        SimpleEntry("pics2/pics2/2.jpg", "EEE")};

    const std::vector<SimpleEntry> source = {
        SimpleEntry("buh.jpg", "AAA"),
        SimpleEntry("img"),
        SimpleEntry("img/1.jpg", "AAA"),
        SimpleEntry("img/pics"),
        SimpleEntry("img/pics/1.jpg", "AAA"),
        SimpleEntry("img/pics/pics"),
        SimpleEntry("img/pics/pics/1.jpg", "AAA"),
        SimpleEntry("asd.jpg", "EEE"),
        SimpleEntry("lol"),
        SimpleEntry("lol/3.jpg", "GGG"),
        SimpleEntry("lol/pics2"),
        SimpleEntry("lol/pics2/2.jpg", "EEE")};

    performDeltaTest(dest, source);
}

TEST(applyDeltaTest, deepTree3) {
    const std::vector<SimpleEntry> dest = {
        SimpleEntry("1.jpg", "AAA"),
        SimpleEntry("pics"),
        SimpleEntry("pics/1.jpg", "AAA"),
        SimpleEntry("pics/pics"),
        SimpleEntry("pics/pics/1.jpg", "AAA"),
        SimpleEntry("pics/pics/pics"),
        SimpleEntry("pics/pics/pics/1.jpg", "AAA"),
        SimpleEntry("pics/pics/pics/2.jpg", "AAA"),
        SimpleEntry("pics/pics/pics/3.jpg", "KKK"),
        SimpleEntry("pics/pics/pics/4.jpg", "III"),
        SimpleEntry("pics/pics/pics/5.jpg", "LLL"),
        SimpleEntry("pics/pics/pics/6.jpg", "VVV"),
        SimpleEntry("pics/pics/pics/7.jpg", "AAA"),
        SimpleEntry("pics/pics/pics/8.jpg", "AAA"),
        SimpleEntry("pics/pics/pics/9.jpg", "HHH"),
        SimpleEntry("2.jpg", "EEE"),
        SimpleEntry("pics2"),
        SimpleEntry("pics2/3.jpg", "GGG"),
        SimpleEntry("pics2/pics2"),
        SimpleEntry("pics2/pics2/2.jpg", "EEE")};

    const std::vector<SimpleEntry> source = {
        SimpleEntry("buh.jpg", "AAA"),
        SimpleEntry("img"),
        SimpleEntry("img/1.jpg", "AAA"),
        SimpleEntry("img/pics"),
        SimpleEntry("img/pics/1.jpg", "AAA"),
        SimpleEntry("img/pics/pics"),
        SimpleEntry("img/pics/pics/1.jpg", "AAA"),
        SimpleEntry("img/lol/pics"),
        SimpleEntry("img/lol/pics/2.jpg", "AAA"),
        SimpleEntry("img/lol/pics/3.jpg", "KKK"),
        SimpleEntry("img/lol/pics/4.jpg", "III"),
        SimpleEntry("img/lol/pics/5.jpg", "LLL"),
        SimpleEntry("img/lol/pics/6.jpg", "VVV"),
        SimpleEntry("img/lol/pics/7.jpg", "AAA"),
        SimpleEntry("img/lol/pics/8.jpg", "AAA"),
        SimpleEntry("img/pics/pics/9.jpg", "HHH"),
        SimpleEntry("img/pics/pics/1.jpg", "AAA"),
        SimpleEntry("asd.jpg", "EEE"),
        SimpleEntry("lol"),
        SimpleEntry("lol/3.jpg", "GGG"),
        SimpleEntry("lol/pics2"),
        SimpleEntry("lol/pics2/2.jpg", "EEE")};

    performDeltaTest(dest, source);
}

TEST(applyDeltaTest, edgeCase) {
    const std::vector<SimpleEntry> dest = {
        SimpleEntry("1.jpg", "AAA"),
        SimpleEntry("pics"),
        SimpleEntry("pics/2.jpg", "BBB"),
        SimpleEntry("pics/pics"),
        SimpleEntry("pics/pics/3.jpg", "CCC"),
        SimpleEntry("pics/pics/pics"),
        SimpleEntry("pics/pics/pics/4.jpg", "DDD")};

    const std::vector<SimpleEntry> source = {
        SimpleEntry("lol"), SimpleEntry("lol/3.jpg", "GGG"),
        SimpleEntry("lol/pics2"), SimpleEntry("lol/pics2/2.jpg", "EEE")};

    performDeltaTest(dest, source);
}

TEST(applyDeltaTest, edgeCase2) {
    const std::vector<SimpleEntry> dest = {
        SimpleEntry("1.jpg", "AAA"),
        SimpleEntry("pics"),
        SimpleEntry("pics/2.jpg", "BBB"),
        SimpleEntry("pics/pics"),
        SimpleEntry("pics/pics/3.jpg", "CCC"),
        SimpleEntry("pics/pics/pics"),
        SimpleEntry("pics/pics/pics/4.jpg", "DDD")};

    const std::vector<SimpleEntry> source = {
        SimpleEntry("1.jpg", "AAA"),
        SimpleEntry("pics"),
        SimpleEntry("pics/2.jpg", "BBB"),
        SimpleEntry("pics/pics"),
        SimpleEntry("pics/pics/3.jpg", "CCC"),
        SimpleEntry("pics/pics/pics"),
        SimpleEntry("pics/pics/pics/4.jpg", "DDD"),
        SimpleEntry("img"),
        SimpleEntry("img/1.jpg", "AAA"),
        SimpleEntry("img/pics"),
        SimpleEntry("img/pics/2.jpg", "BBB"),
        SimpleEntry("img/pics/pics"),
        SimpleEntry("img/pics/pics/3.jpg", "CCC"),
        SimpleEntry("img/pics/pics/pics"),
        SimpleEntry("img/pics/pics/pics/4.jpg", "DDD")

    };

    performDeltaTest(dest, source);
}

TEST(applyDeltaTest, edgeCase3) {
    const std::vector<SimpleEntry> dest = {
        SimpleEntry("1.jpg", "AAA"),
        SimpleEntry("pics"),
        SimpleEntry("pics/2.jpg", "BBB"),
        SimpleEntry("pics/pics"),
        SimpleEntry("pics/pics/3.jpg", "CCC"),
        SimpleEntry("pics/pics/pics"),
        SimpleEntry("pics/pics/pics/4.jpg", "DDD"),
        SimpleEntry("img"),
        SimpleEntry("img/1.jpg", "AAA"),
        SimpleEntry("img/pics"),
        SimpleEntry("img/pics/2.jpg", "BBB"),
        SimpleEntry("img/pics/pics"),
        SimpleEntry("img/pics/pics/3.jpg", "CCC"),
        SimpleEntry("img/pics/pics/pics"),
        SimpleEntry("img/pics/pics/pics/4.jpg", "DDD")};

    const std::vector<SimpleEntry> source = {
        SimpleEntry("1.jpg", "AAA"),
        SimpleEntry("pics"),
        SimpleEntry("pics/2.jpg", "BBB"),
        SimpleEntry("pics/pics"),
        SimpleEntry("pics/pics/3.jpg", "CCC"),
        SimpleEntry("pics/pics/pics"),
        SimpleEntry("pics/pics/pics/4.jpg", "DDD")};

    performDeltaTest(dest, source);
}

TEST(applyDeltaTest, hardNames) {
    const std::vector<SimpleEntry> dest = {
        SimpleEntry("a", "AA"),
        SimpleEntry("b", "BB"),
    };

    const std::vector<SimpleEntry> source = {SimpleEntry("a"), SimpleEntry("b"),
                                             SimpleEntry("b/a", "DD"),
                                             SimpleEntry("b/c", "BB")};

    performDeltaTest(dest, source);
}

TEST(applyDeltaTest, hardNames2) {
    const std::vector<SimpleEntry> dest = {
        SimpleEntry("a", "AA"), SimpleEntry("b", "BB"),
        SimpleEntry("c"),       SimpleEntry("c/a", "AA"),
        SimpleEntry("d"),       SimpleEntry("d/b", "BB"),

    };

    const std::vector<SimpleEntry> source = {
        SimpleEntry("a"),         SimpleEntry("b"), SimpleEntry("b/a", "DD"),
        SimpleEntry("b/c", "BB"), SimpleEntry("c"), SimpleEntry("d"),

    };

    performDeltaTest(dest, source);
}

TEST(applyDeltaTest, edgeCase4) {
    const std::vector<SimpleEntry> dest = {
        SimpleEntry(
            "20200830_160445.jpg",
            "8a407cc1322d0840a0abe983e76637c4db99af5ee4923df8c186035715145854"),
        SimpleEntry(
            "20200830_160447.jpg",
            "ab325a4ffc7b7122744c01159fe54c55e71da80c15e67deddf2d14694cf20950"),
        SimpleEntry(
            "20200830_160451.jpg",
            "0168f7339744c8c1bef616e8c31e0834d72b87974ba3c5712c8fe1ddb2a1b6a5"),
        SimpleEntry(
            "DJI_0007.JPG",
            "0c855a1659f63c68be27c3608b0fd4ad94d9ffb1f3cc1ce7d88e4fc4afb2c7fe"),
        SimpleEntry(
            "DJI_0008.JPG",
            "62921492a63fcac1bac18d5a84d2cfd43d0670c1f4748d1ad7783cdd30afbf19"),
        SimpleEntry(
            "DJI_0009.JPG",
            "b919a6a019aa0949401a85ea0b54a49c84f4e4f9c1a77b50c1603ab7dd92a699"),
        SimpleEntry(
            "DJI_0035.JPG",
            "777beb12669b654c82f0a9c8690173b01093ee636d63052e55947a2c256452c8"),
        SimpleEntry(
            "DJI_0036.JPG",
            "33068e5996983fe652af862c57ecae814ad78d66e76a77e0fd6c787ae4591bb6"),
        SimpleEntry(
            "DJI_0037.JPG",
            "13ec3651527dd1a3bdfbfb4d5b17748a474f8a03757e9e8becd45f413a6db851"),
        SimpleEntry(
            "odm_orthophoto.tif",
            "bc4bd1d51581baa203af8a45af023a5fa192a840fb6a99ceb510039a3c3d4c9a"),
        SimpleEntry(
            "testhub.bat",
            "f6edae6c0986ca2353a51008246eb8b8ff26ecb77dc331e7e0b9ebb22b33b038"),
        SimpleEntry(
            "Suba/DJI_0048.JPG",
            "25a1851f87f3c3d3c323b6a5db630b12756d9cf7fb5e15ed8a945cd5fdf3a0c5"),
        SimpleEntry(
            "Suba/Sub2/DJI_0051.JPG",
            "3613595328c07a52bcc9e160c1cb54e8209f8b097545bdd7d45cba71687bd89f"),
        SimpleEntry(
            "Suba/Sub2/DJI_0052.JPG",
            "f4c994a067865d47b6bd6a1c1a9c6dfcb913861355b477aa781437c5a6d44144"),
        SimpleEntry(
            "Suba/Sub3/DJI_0050.JPG",
            "29f02abdec2c5c515376f432aef2939f66ba31f76750d108f174ab99fd30f96d"),
        SimpleEntry("Suba"),
        SimpleEntry("Suba/Sub2"),
        SimpleEntry("Suba/Sub3")};

    const std::vector<SimpleEntry> source = {
        SimpleEntry(
            "20200830_160445.jpg",
            "8a407cc1322d0840a0abe983e76637c4db99af5ee4923df8c186035715145854"),
        SimpleEntry(
            "20200830_160447.jpg",
            "ab325a4ffc7b7122744c01159fe54c55e71da80c15e67deddf2d14694cf20950"),
        SimpleEntry(
            "20200830_160451.jpg",
            "0168f7339744c8c1bef616e8c31e0834d72b87974ba3c5712c8fe1ddb2a1b6a5"),
        SimpleEntry(
            "DJI_0007.JPG",
            "0c855a1659f63c68be27c3608b0fd4ad94d9ffb1f3cc1ce7d88e4fc4afb2c7fe"),
        SimpleEntry(
            "DJI_0008.JPG",
            "62921492a63fcac1bac18d5a84d2cfd43d0670c1f4748d1ad7783cdd30afbf19"),
        SimpleEntry(
            "DJI_0009.JPG",
            "b919a6a019aa0949401a85ea0b54a49c84f4e4f9c1a77b50c1603ab7dd92a699"),
        SimpleEntry(
            "DJI_0035.JPG",
            "777beb12669b654c82f0a9c8690173b01093ee636d63052e55947a2c256452c8"),
        SimpleEntry(
            "DJI_0036.JPG",
            "33068e5996983fe652af862c57ecae814ad78d66e76a77e0fd6c787ae4591bb6"),
        SimpleEntry(
            "DJI_0037.JPG",
            "13ec3651527dd1a3bdfbfb4d5b17748a474f8a03757e9e8becd45f413a6db851"),

        SimpleEntry(
            "DJI_0038.JPG",
            "85ad36d56fa6a1c904872ae4a1272b7a541e8e3184d6c7eae9f3479ff1b24806"),
        SimpleEntry(
            "DJI_0039.JPG",
            "77d7d649f5372d61ae3a2317cfe629ff56492cb395891669d4aacdde8d831994"),
        SimpleEntry(
            "localhost.bat",
            "4a13712f6efaa85db9f2157828a1eddc3890256a4e439b89aaa5aa370d1b9003"),

        SimpleEntry(
            "odm_orthophoto.tif",
            "bc4bd1d51581baa203af8a45af023a5fa192a840fb6a99ceb510039a3c3d4c9a"),
        SimpleEntry(
            "testhub.bat",
            "f6edae6c0986ca2353a51008246eb8b8ff26ecb77dc331e7e0b9ebb22b33b038"),
        SimpleEntry(
            "Sub/DJI_0048.JPG",
            "25a1851f87f3c3d3c323b6a5db630b12756d9cf7fb5e15ed8a945cd5fdf3a0c5"),
        SimpleEntry(
            "Sub/DJI_0049.JPG",
            "7dbc0bee5d5ffb0dc389bb4d611be6639fb52f1d3346502c0f8a1a486cc8c19e"),
        SimpleEntry(
            "Sub/Sub2/DJI_0051.JPG",
            "3613595328c07a52bcc9e160c1cb54e8209f8b097545bdd7d45cba71687bd89f"),
        SimpleEntry(
            "Sub/Sub2/DJI_0052.JPG",
            "f4c994a067865d47b6bd6a1c1a9c6dfcb913861355b477aa781437c5a6d44144"),
        SimpleEntry(
            "Sub/Sub2/DJI_0053.JPG",
            "480d7c37fd970fa4ac4389667b8cec405608e7a0e3a0ad5d021ca79f91ddc7c1"),
        SimpleEntry(
            "Sub/Sub3/DJI_0050.JPG",
            "29f02abdec2c5c515376f432aef2939f66ba31f76750d108f174ab99fd30f96d"),
        SimpleEntry("Sub"),
        SimpleEntry("Sub/Sub2"),
        SimpleEntry("Sub/Sub3")};

    performDeltaTest(dest, source);
}

}  // namespace
