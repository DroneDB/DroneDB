/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Nexus files are written by ddb-nexus, a program of its own, so the Nexus code never lives in
// a host process. Running it in-process once crashed hosts: one that ran the build on a worker
// thread (Registry: a Hangfire job) died with SIGSEGV when the process ended, because the
// application object the Nexus code created was destroyed by exit() on another thread than the
// one that created it. These tests check what replaces it: the build from a worker thread
// works, nothing of Nexus is loaded into the process, and a tool that is missing or fails is an
// AppException that says why.

#include <fstream>
#include <string>
#include <thread>

#include "3d.h"
#include "exceptions.h"
#include "fs.h"
#include "gtest/gtest.h"
#include "helpers/process_test_utils.h"
#include "mio.h"
#include "nexus_runner.h"
#include "process_runner.h"
#include "test.h"
#include "testarea.h"
#include "tool_locator.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    // A single triangle: the smallest model the build accepts
    fs::path writeModel(TestArea &ta)
    {
        const fs::path obj = ta.getPath("model.obj");
        std::ofstream(obj) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        return obj;
    }

    // buildNexus with the tool the variable names (a fake: ddbtest_child), looked up again
    std::string buildWithTool(TestArea &ta, const fs::path &tool)
    {
        EnvVarGuard toolPath("DDB_NEXUS_PATH", io::toUtf8(tool));
        nexus::findNexusBinary(true);
        std::string message;
        try
        {
            buildNexus(io::toUtf8(writeModel(ta)), io::toUtf8(ta.getPath("model.nxz")), true);
            ADD_FAILURE() << "the build should have failed";
        }
        catch (const AppException &e)
        {
            message = e.what();
        }
        return message;
    }

    bool processHasLoaded(const std::string &name)
    {
        std::ifstream maps("/proc/self/maps");
        for (std::string line; std::getline(maps, line);)
            if (line.find(name) != std::string::npos)
                return true;
        return false;
    }

    void expectBuilt(const std::string &nxz)
    {
        std::error_code ec;
        EXPECT_TRUE(fs::is_regular_file(io::fromUtf8(nxz), ec)) << nxz;
        EXPECT_GT(fs::file_size(io::fromUtf8(nxz), ec), 0u) << nxz;
    }

    // The control: the build on the calling thread
    TEST(NexusProcess, buildOnTheMainThread)
    {
        TestArea ta(TEST_NAME, true);
        expectBuilt(buildNexus(io::toUtf8(writeModel(ta)), io::toUtf8(ta.getPath("model.nxz")), true));
    }

    // The thread that built is gone when the process ends: nothing it created is left to be
    // destroyed on another thread
    TEST(NexusProcess, buildOnAWorkerThreadThatThenEnds)
    {
        TestArea ta(TEST_NAME, true);
        const std::string model = io::toUtf8(writeModel(ta));
        const std::string nxz = io::toUtf8(ta.getPath("model.nxz"));
        std::string built;
        std::thread([&] { built = buildNexus(model, nxz, true); }).join();
        expectBuilt(built);
    }

    TEST(NexusProcess, theBuildLoadsNoNexusCodeIntoThisProcess)
    {
#ifndef __linux__
        GTEST_SKIP() << "reads /proc/self/maps";
#else
        TestArea ta(TEST_NAME, true);
        expectBuilt(buildNexus(io::toUtf8(writeModel(ta)), io::toUtf8(ta.getPath("model.nxz")), true));
        EXPECT_FALSE(processHasLoaded("libnxs"));
#endif
    }

    // ddb-nexus keeps at most --threads N threads busy: a count it cannot use is a usage error
    // (exit 64) that names the option, and a count of 1 builds the model
    TEST(NexusProcess, theToolTakesAThreadCountOfOneOrMore)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path tool = nexus::findNexusBinary(true);
        ASSERT_TRUE(tools::isExecutableFile(tool)) << io::toUtf8(tool);
        const std::string model = io::toUtf8(writeModel(ta));
        const std::string out = io::toUtf8(ta.getPath("model.nxz"));
        const auto &runner = process::systemProcessRunner();

        for (const char *bad : {"0", "-1", "two", "99999"})
        {
            const process::Result r = runner.run(tool, {"--threads", bad, model, out}, process::Options());
            EXPECT_EQ(r.exitCode, 64) << bad;
            EXPECT_NE(r.outputTail.find("--threads"), std::string::npos) << r.outputTail;
            EXPECT_FALSE(fs::exists(ta.getPath("model.nxz"))) << bad;
        }

        const process::Result r = runner.run(tool, {"--threads", "1", model, out}, process::Options());
        EXPECT_EQ(r.exitCode, 0) << r.outputTail;
        expectBuilt(out);
    }

    TEST(NexusProcess, aMissingToolNamesItAndTheVariable)
    {
        TestArea ta(TEST_NAME, true);
        const std::string message = buildWithTool(ta, ta.getPath("missing/ddb-nexus"));
        EXPECT_NE(message.find("ddb-nexus"), std::string::npos) << message;
        EXPECT_NE(message.find("DDB_NEXUS_PATH"), std::string::npos) << message;
        EXPECT_FALSE(fs::exists(ta.getPath("model.nxz")));
        nexus::findNexusBinary(true);
    }

    TEST(NexusProcess, aToolThatFailsIsReportedWithItsStatusAndItsLastOutput)
    {
        TestArea ta(TEST_NAME, true);
        EnvVarGuard mode("DDB_TEST_CHILD_MODE", "fail");
        const std::string message = buildWithTool(ta, testChildPath());
        EXPECT_NE(message.find("ddb-nexus exited with code 1"), std::string::npos) << message;
        EXPECT_NE(message.find("boom: the tool rejected its input"), std::string::npos) << message;
        EXPECT_FALSE(fs::exists(ta.getPath("model.nxz")));
        nexus::findNexusBinary(true);
    }

    TEST(NexusProcess, aToolThatWritesNothingIsAFailure)
    {
        TestArea ta(TEST_NAME, true);
        EnvVarGuard mode("DDB_TEST_CHILD_MODE", "no-output");
        const std::string message = buildWithTool(ta, testChildPath());
        EXPECT_NE(message.find("wrote no output file"), std::string::npos) << message;
        nexus::findNexusBinary(true);
    }
} // namespace
