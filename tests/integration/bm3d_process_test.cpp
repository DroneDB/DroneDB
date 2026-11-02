/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// bm3d files are written by bm3d, a program of its own, so its code never lives in a host
// process. Running such code in-process once crashed hosts: one that ran the build on a worker
// thread (Registry: a Hangfire job) died with SIGSEGV when the process ended, because an
// application object the library code created was destroyed by exit() on another thread than the
// one that created it. These tests check what replaces it: the build from a worker thread
// works, and a tool that is missing or fails is an AppException that says why. That no bm3d code
// is linked into libddb is the job of the GPL linkage check (scripts/check-gpl-linkage.py).

#include <fstream>
#include <string>
#include <thread>

#include "3d.h"
#include "bm3d_runner.h"
#include "exceptions.h"
#include "fs.h"
#include "gtest/gtest.h"
#include "helpers/process_test_utils.h"
#include "mio.h"
#include "process_runner.h"
#include "test.h"
#include "testarea.h"
#include "tool_locator.h"
#include "utils.h"

namespace {
using namespace ddb;

// A single triangle: the smallest model the build accepts
fs::path writeModel(TestArea& ta) {
    const fs::path obj = ta.getPath("model.obj");
    std::ofstream(obj) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    return obj;
}

// buildBm3d with the tool the variable names (a fake: ddbtest_child), looked up again
std::string buildWithTool(TestArea& ta, const fs::path& tool) {
    EnvVarGuard toolPath(bm3d::kBm3dEnv, io::toUtf8(tool));
    bm3d::findBm3dBinary(true);
    std::string message;
    try {
        buildBm3d(io::toUtf8(writeModel(ta)), io::toUtf8(ta.getPath("model.bm3d")), true);
        ADD_FAILURE() << "the build should have failed";
    } catch (const AppException& e) {
        message = e.what();
    }
    return message;
}

void expectBuilt(const std::string& file) {
    std::error_code ec;
    EXPECT_TRUE(fs::is_regular_file(io::fromUtf8(file), ec)) << file;
    EXPECT_GT(fs::file_size(io::fromUtf8(file), ec), 0u) << file;
}

// The control: the build on the calling thread
TEST(Bm3dProcess, buildOnTheMainThread) {
    TestArea ta(TEST_NAME, true);
    expectBuilt(buildBm3d(io::toUtf8(writeModel(ta)), io::toUtf8(ta.getPath("model.bm3d")), true));
}

// The thread that built is gone when the process ends: nothing it created is left to be
// destroyed on another thread
TEST(Bm3dProcess, buildOnAWorkerThreadThatThenEnds) {
    TestArea ta(TEST_NAME, true);
    const std::string model = io::toUtf8(writeModel(ta));
    const std::string output = io::toUtf8(ta.getPath("model.bm3d"));
    std::string built;
    std::thread([&] { built = buildBm3d(model, output, true); }).join();
    expectBuilt(built);
}

TEST(Bm3dProcess, aMissingToolNamesItAndTheVariable) {
    TestArea ta(TEST_NAME, true);
    const std::string message = buildWithTool(ta, ta.getPath("missing/bm3d"));
    EXPECT_NE(message.find("bm3d"), std::string::npos) << message;
    EXPECT_NE(message.find("DDB_BM3D_PATH"), std::string::npos) << message;
    EXPECT_FALSE(fs::exists(ta.getPath("model.bm3d")));
    bm3d::findBm3dBinary(true);
}

TEST(Bm3dProcess, aToolThatFailsIsReportedWithItsStatusAndItsLastOutput) {
    TestArea ta(TEST_NAME, true);
    EnvVarGuard mode("DDB_TEST_CHILD_MODE", "fail");
    const std::string message = buildWithTool(ta, testChildPath());
    EXPECT_NE(message.find("bm3d exited with code 1"), std::string::npos) << message;
    EXPECT_NE(message.find("boom: the tool rejected its input"), std::string::npos) << message;
    EXPECT_FALSE(fs::exists(ta.getPath("model.bm3d")));
    bm3d::findBm3dBinary(true);
}

TEST(Bm3dProcess, aToolThatWritesNothingIsAFailure) {
    TestArea ta(TEST_NAME, true);
    EnvVarGuard mode("DDB_TEST_CHILD_MODE", "no-output");
    const std::string message = buildWithTool(ta, testChildPath());
    EXPECT_NE(message.find("wrote no output file"), std::string::npos) << message;
    bm3d::findBm3dBinary(true);
}
}  // namespace
