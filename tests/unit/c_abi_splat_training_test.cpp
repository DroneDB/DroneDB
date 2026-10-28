/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDBExportSplatTrainingData: the training data of a splat from the ODM project that was uploaded
// to a dataset. It is an export new in 2.0, so an invalid argument is DDBERR_INVALIDARGS, a folder
// that is not in the dataset DDBERR_NOTFOUND and a stop asked by the progress callback or a cancel
// token DDBERR_CANCELED; the output is written on DDBERR_NONE only. What the files hold is the
// matter of splat_training_export_test.cpp.

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "ddb.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "odm_project_support.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

    // A dataset that holds an ODM project in odm/
    struct Dataset
    {
        TestArea area;
        fs::path root;
        fs::path out;

        explicit Dataset(const std::string &name) : area(name, true), root(area.getFolder("ds")), out(area.getPath("out"))
        {
            char *path = nullptr;
            EXPECT_EQ(DDBInit(root.string().c_str(), &path), DDBERR_NONE) << DDBGetLastError();
            DDBFree(path);
            testutils::writeOdmProject(root / "odm", testutils::defaultOdmProject());
        }

        // Exports into a fresh `out`: the export refuses a folder that already holds files
        DDBErr exportFolder(const char *folder, const char *options, std::string &json, DDBProgressCallback progress = nullptr,
                            void *userData = nullptr)
        {
            fs::remove_all(out);
            return exportTo(out, folder, options, json, progress, userData);
        }

        DDBErr exportTo(const fs::path &outFolder, const char *folder, const char *options, std::string &json,
                        DDBProgressCallback progress = nullptr, void *userData = nullptr)
        {
            char *output = nullptr;
            const DDBErr code = DDBExportSplatTrainingData(root.string().c_str(), folder, outFolder.string().c_str(), options,
                                                           progress, userData, &output);
            json = output != nullptr ? output : "";
            DDBFree(output);
            return code;
        }
    };

    std::string contentOf(const fs::path &file)
    {
        std::ifstream in(file, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    // The files under folder this process holds open (Linux, which lists them in /proc/self/fd;
    // elsewhere none are reported)
    std::vector<std::string> openFilesUnder(const fs::path &folder)
    {
        std::vector<std::string> open;
#ifdef __linux__
        const std::string prefix = fs::weakly_canonical(folder).string() + "/";
        std::error_code ec;
        for (const auto &fd : fs::directory_iterator("/proc/self/fd", ec))
        {
            std::error_code linkError;
            const std::string target = fs::read_symlink(fd.path(), linkError).string();
            if (!linkError && target.starts_with(prefix))
                open.push_back(target);
        }
#else
        (void)folder;
#endif
        return open;
    }

    TEST(CAbiSplatTraining, exportsTheProjectOfAFolderOfTheDataset)
    {
        Dataset d(TEST_NAME);
        std::string text;
        ASSERT_DDB_OK(d.exportFolder("odm", nullptr, text));

        const json result = json::parse(text);
        EXPECT_EQ(result["warnings"], json::array());
        EXPECT_EQ(result["images"], 2);
        EXPECT_EQ(result["points"], 4);
        EXPECT_TRUE(fs::is_regular_file(d.out / "sparse" / "0" / "cameras.bin"));
        EXPECT_TRUE(fs::is_regular_file(d.out / "sparse" / "0" / "images.bin"));
        EXPECT_TRUE(fs::is_regular_file(d.out / "sparse" / "0" / "points3D.bin"));
        EXPECT_TRUE(fs::is_regular_file(d.out / "images" / "a.JPG"));
        EXPECT_TRUE(fs::is_regular_file(d.out / "georef.json"));
    }

    TEST(CAbiSplatTraining, takesTheRootOfTheDatasetForNoFolderAndAcceptsBackslashes)
    {
        Dataset d(TEST_NAME);
        testutils::writeOdmProject(d.root, testutils::defaultOdmProject());
        std::string text;
        for (const char *folder : {static_cast<const char *>(nullptr), "", "."})
        {
            ASSERT_DDB_OK(d.exportFolder(folder, nullptr, text)) << (folder ? folder : "NULL");
            EXPECT_EQ(json::parse(text)["images"], 2);
        }

        fs::create_directories(d.root / "a" / "b");
        testutils::writeOdmProject(d.root / "a" / "b", testutils::defaultOdmProject());
        ASSERT_DDB_OK(d.exportFolder("a\\b", nullptr, text));
        EXPECT_EQ(json::parse(text)["images"], 2);
    }

    TEST(CAbiSplatTraining, appliesTheOptions)
    {
        Dataset d(TEST_NAME);
        std::string text;
        ASSERT_DDB_OK(d.exportFolder("odm", R"({"maxImageSize": 64, "maxPoints": 2})", text));
        EXPECT_LE(json::parse(text)["points"].get<int>(), 2);
        EXPECT_EQ(openFilesUnder(d.out), std::vector<std::string>())
            << "a file of the export left open could be neither replaced nor removed on Windows";

        {
            // Closed before the next export, which removes the output folder
            const auto ds = adoptDataset(GDALOpen((d.out / "images" / "a.JPG").string().c_str(), GA_ReadOnly));
            ASSERT_TRUE(ds);
            EXPECT_EQ(GDALGetRasterXSize(datasetHandle(ds)), 64);
            EXPECT_EQ(GDALGetRasterYSize(datasetHandle(ds)), 48);
        }

        for (const char *none : {"", "{}"})
            ASSERT_DDB_OK(d.exportFolder("odm", none, text)) << none;
        EXPECT_EQ(json::parse(text)["points"], 4);
    }

    TEST(CAbiSplatTraining, refusesOptionsItDoesNotKnowOrThatAreNotWholeNumbers)
    {
        Dataset d(TEST_NAME);
        std::string text;
        for (const char *options : {"[]", "7", "{", R"({"threads": 2})", R"({"maxImageSize": -1})", R"({"maxImageSize": 2.0})",
                                    R"({"maxImageSize": "64"})", R"({"maxPoints": true})", R"({"maxPoints": 1.5})",
                                    R"({"maxImageSize": 99999999999})"})
        {
            SCOPED_TRACE(options);
            EXPECT_EQ(d.exportFolder("odm", options, text), DDBERR_INVALIDARGS);
            EXPECT_STRNE(DDBGetLastError(), "");
            EXPECT_TRUE(text.empty());
        }
        EXPECT_FALSE(fs::exists(d.out)) << "nothing is written for an option that is refused";
    }

    TEST(CAbiSplatTraining, failsWithTheCodeOfWhatIsWrong)
    {
        Dataset d(TEST_NAME);
        std::string text;

        EXPECT_EQ(d.exportFolder("nothing-here", nullptr, text), DDBERR_NOTFOUND);
        EXPECT_NE(std::string(DDBGetLastError()).find("nothing-here"), std::string::npos);
        EXPECT_EQ(d.exportFolder("odm/images/a.JPG", nullptr, text), DDBERR_NOTFOUND) << "a file is not a folder";

        EXPECT_EQ(d.exportFolder("../odm", nullptr, text), DDBERR_INVALIDARGS);
        EXPECT_EQ(d.exportFolder("/odm", nullptr, text), DDBERR_INVALIDARGS);
        EXPECT_EQ(d.exportFolder(".ddb", nullptr, text), DDBERR_INVALIDARGS);

        fs::remove(d.root / "odm" / "odm_georeferencing" / "coords.txt");
        EXPECT_EQ(d.exportFolder("odm", nullptr, text), DDBERR_EXCEPTION);
        EXPECT_NE(std::string(DDBGetLastError()).find("coords.txt"), std::string::npos) << DDBGetLastError();
        EXPECT_TRUE(text.empty());
    }

    TEST(CAbiSplatTraining, refusesAPointLimitAboveItsCap)
    {
        Dataset d(TEST_NAME);
        std::string text;
        EXPECT_EQ(d.exportFolder("odm", R"({"maxPoints": 10000001})", text), DDBERR_INVALIDARGS);
        EXPECT_NE(std::string(DDBGetLastError()).find("10000000"), std::string::npos) << DDBGetLastError();
        EXPECT_FALSE(fs::exists(d.out));
        ASSERT_DDB_OK(d.exportFolder("odm", R"({"maxPoints": 10000000})", text));
        EXPECT_EQ(json::parse(text)["points"], 4);
    }

    // Written into the dataset, the resized images would replace its photos of the same name and
    // its georef.json would georeference the models beside it at the next index
    TEST(CAbiSplatTraining, refusesAnOutputFolderInTheDatasetAndChangesNothing)
    {
        Dataset d(TEST_NAME);
        const fs::path photo = d.root / "images" / "a.JPG";
        fs::create_directories(photo.parent_path());
        fs::copy_file(d.root / "odm" / "images" / "a.JPG", photo);
        const std::string original = contentOf(photo);

        std::string text;
        const auto refused = [&](const fs::path &out) {
            SCOPED_TRACE(out.string());
            EXPECT_EQ(d.exportTo(out, "odm", R"({"maxImageSize": 64})", text), DDBERR_INVALIDARGS);
            EXPECT_NE(std::string(DDBGetLastError()).find("in the dataset"), std::string::npos) << DDBGetLastError();
            EXPECT_TRUE(text.empty());
        };
        refused(d.root);
        refused(d.root / ".");
        refused(d.root / "exports" / "training");
        refused(d.root / ".." / d.root.filename() / "training");

        // A link to a folder of the dataset is the dataset
        std::error_code ec;
        fs::create_directory_symlink(d.root, d.area.getPath("link"), ec);
        if (!ec)
            refused(d.area.getPath("link") / "training");

        EXPECT_EQ(contentOf(photo), original) << "a photo of the dataset was replaced";
        EXPECT_FALSE(fs::exists(d.root / "georef.json"));
        EXPECT_FALSE(fs::exists(d.root / "sparse"));
        EXPECT_FALSE(fs::exists(d.root / "exports"));
        EXPECT_FALSE(fs::exists(d.root / "training"));
    }

    TEST(CAbiSplatTraining, refusesAnOutputFolderThatIsNotEmpty)
    {
        Dataset d(TEST_NAME);
        fs::create_directories(d.out / "images");
        testutils::writeTextFile(d.out / "images" / "a.JPG", "keep me");

        std::string text;
        EXPECT_EQ(d.exportTo(d.out, "odm", nullptr, text), DDBERR_INVALIDARGS);
        EXPECT_NE(std::string(DDBGetLastError()).find("not empty"), std::string::npos) << DDBGetLastError();
        EXPECT_EQ(contentOf(d.out / "images" / "a.JPG"), "keep me");
        EXPECT_FALSE(fs::exists(d.out / "georef.json"));

        // An empty one is fine
        fs::remove_all(d.out);
        fs::create_directories(d.out);
        ASSERT_DDB_OK(d.exportTo(d.out, "odm", nullptr, text));
    }

    TEST(CAbiSplatTraining, refusesMissingArguments)
    {
        Dataset d(TEST_NAME);
        const std::string root = d.root.string(), out = d.out.string();
        char *output = nullptr;
        EXPECT_EQ(DDBExportSplatTrainingData(nullptr, "odm", out.c_str(), nullptr, nullptr, nullptr, &output), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBExportSplatTrainingData("", "odm", out.c_str(), nullptr, nullptr, nullptr, &output), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBExportSplatTrainingData(root.c_str(), "odm", nullptr, nullptr, nullptr, nullptr, &output), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBExportSplatTrainingData(root.c_str(), "odm", "", nullptr, nullptr, nullptr, &output), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBExportSplatTrainingData(root.c_str(), "odm", out.c_str(), nullptr, nullptr, nullptr, nullptr), DDBERR_INVALIDARGS);
        EXPECT_EQ(output, nullptr);
        EXPECT_FALSE(fs::exists(d.out));
    }

    // What the progress callback was called with
    struct Seen
    {
        std::vector<std::string> phases;
        std::string stopAt;
        double last = -1.0;
        bool decreased = false;
    };

    int record(double fraction, const char *phase, void *userData)
    {
        auto *seen = static_cast<Seen *>(userData);
        seen->phases.push_back(phase);
        seen->decreased = seen->decreased || fraction < seen->last;
        seen->last = fraction;
        return phase == seen->stopAt ? 1 : 0;
    }

    TEST(CAbiSplatTraining, reportsProgressAndStopsWhenTheCallbackAsksTo)
    {
        Dataset d(TEST_NAME);
        std::string text;
        Seen seen;
        ASSERT_DDB_OK(d.exportFolder("odm", nullptr, text, &record, &seen));
        EXPECT_FALSE(seen.decreased);
        EXPECT_EQ(seen.last, 1.0);
        EXPECT_EQ(seen.phases.front(), "cameras");
        EXPECT_EQ(seen.phases.back(), "model");

        for (const char *phase : {"cameras", "points", "images"})
        {
            SCOPED_TRACE(phase);
            fs::remove_all(d.out);
            Seen stopping;
            stopping.stopAt = phase;
            EXPECT_EQ(d.exportFolder("odm", nullptr, text, &record, &stopping), DDBERR_CANCELED);
            EXPECT_STRNE(DDBGetLastError(), "");
            EXPECT_TRUE(text.empty());
            EXPECT_FALSE(fs::exists(d.out / "sparse" / "0" / "cameras.bin"));
        }
    }

    TEST(CAbiSplatTraining, isListedAsCancellable)
    {
        const json caps = json::parse(DDBGetCapabilities());
        const auto &cancellable = caps["cancellable"];
        EXPECT_NE(std::find(cancellable.begin(), cancellable.end(), "DDBExportSplatTrainingData"), cancellable.end());
    }
} // namespace
