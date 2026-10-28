/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// ddb gsplat --training-data <output folder> <ODM project folder>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cmd/include/cli.h"
#include "cmd/include/cmdlist.h"
#include "gtest/gtest.h"
#include "local_frame_georef.h"
#include "mio.h"
#include "odm_project_support.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace cmd;

    struct Outcome
    {
        int code = -1;
        std::string out; // stdout
        std::string err; // stderr (the warnings) and the handler's messages
    };

    Outcome gsplat(std::vector<std::string> args)
    {
        auto command = CommandRegistry::builtin().create("gsplat");
        std::vector<char *> argv{const_cast<char *>("ddb")};
        for (auto &a : args)
            argv.push_back(a.data());

        std::ostringstream out, err, handlerErr;
        std::streambuf *previousOut = std::cout.rdbuf(out.rdbuf());
        std::streambuf *previousErr = std::cerr.rdbuf(err.rdbuf());
        Outcome outcome;
        try
        {
            outcome.code = cmd::execute(*command, static_cast<int>(argv.size()), argv.data(), handlerErr);
        }
        catch (...)
        {
            std::cout.rdbuf(previousOut);
            std::cerr.rdbuf(previousErr);
            throw;
        }
        std::cout.rdbuf(previousOut);
        std::cerr.rdbuf(previousErr);
        outcome.out = out.str();
        outcome.err = err.str() + handlerErr.str();
        return outcome;
    }

    TEST(GsplatTrainingDataCli, writesTheTrainingDataAndPrintsTheFolder)
    {
        TestArea area(TEST_NAME, true);
        const auto project = testutils::writeOdmProject(area.getFolder("odm"), testutils::defaultOdmProject());
        const auto out = area.getPath("out");

        const auto outcome = gsplat({"--training-data", out.string(), project.string()});
        EXPECT_EQ(outcome.code, EXIT_SUCCESS) << outcome.err;
        EXPECT_EQ(outcome.out, ddb::io::toUtf8(out) + "\n");
        EXPECT_NE(outcome.err.find("2 images, 4 points"), std::string::npos) << outcome.err;
        EXPECT_TRUE(std::filesystem::is_regular_file(out / "sparse" / "0" / "cameras.bin"));
        EXPECT_TRUE(std::filesystem::is_regular_file(out / "images" / "a.JPG"));
        EXPECT_TRUE(ddb::detectLocalFrameGeoref(ddb::io::toUtf8(out / "splat.ply")).has_value());
    }

    TEST(GsplatTrainingDataCli, takesTheSizeAndThePointsAndPrintsTheWarnings)
    {
        TestArea area(TEST_NAME, true);
        auto spec = testutils::defaultOdmProject();
        spec.cloud = testutils::OdmCloud::None;
        const auto project = testutils::writeOdmProject(area.getFolder("odm"), spec);
        const auto out = area.getPath("out");

        const auto outcome =
            gsplat({"--training-data", out.string(), "--max-image-size", "64", "--max-points", "10", project.string()});
        EXPECT_EQ(outcome.code, EXIT_SUCCESS) << outcome.err;
        EXPECT_NE(outcome.err.find("Warning: The ODM project has no point cloud"), std::string::npos) << outcome.err;
    }

    TEST(GsplatTrainingDataCli, failsNamingTheMissingFile)
    {
        TestArea area(TEST_NAME, true);
        const auto project = testutils::writeOdmProject(area.getFolder("odm"), testutils::defaultOdmProject());
        std::filesystem::remove(project / "odm_georeferencing" / "coords.txt");

        const auto outcome = gsplat({"--training-data", area.getPath("out").string(), project.string()});
        EXPECT_EQ(outcome.code, EXIT_FAILURE);
        EXPECT_TRUE(outcome.out.empty()) << outcome.out;
        EXPECT_NE(outcome.err.find("coords.txt"), std::string::npos) << outcome.err;
        EXPECT_EQ(outcome.err.find("Usage:"), std::string::npos) << "a missing file is not a usage error: " << outcome.err;
    }

    TEST(GsplatTrainingDataCli, sizeAndPointsGoWithTrainingData)
    {
        TestArea area(TEST_NAME, true);
        const auto outcome = gsplat({"--max-points", "10", area.getPath("scene.ply").string()});
        EXPECT_EQ(outcome.code, EXIT_FAILURE);
        EXPECT_NE(outcome.err.find("--training-data"), std::string::npos) << outcome.err;

        const auto withOutput = gsplat({"--training-data", area.getPath("out").string(), "-o", "x.spz", "project"});
        EXPECT_EQ(withOutput.code, EXIT_FAILURE);
    }
} // namespace
