/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A host that runs the Nexus build on a worker thread (Registry: a Hangfire job) crashed with
// SIGSEGV when the process ended, after the build had succeeded: libnexus kept its
// QCoreApplication in a static that exit() destroyed on another thread than the one that created
// it. The ddb CLI builds on its main thread and never saw it. The probe
// (tests/helpers/nexus_probe.cpp) is its own process, because the crash is in exit().

#include "fs.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "process_runner.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    process::Result runProbe(const std::string &mode, const std::string &testName)
    {
        const fs::path probe = io::getExeFolderPath() / DDB_NEXUS_PROBE_NAME;
        EXPECT_TRUE(fs::exists(probe)) << probe;
        TestArea ta(testName, true);
        return process::run(probe, {mode, io::toUtf8(ta.getFolder())});
    }

    // The control: the build and the process exit on the main thread
    TEST(NexusExit, buildOnMainThread)
    {
        const process::Result r = runProbe("main", TEST_NAME);
        EXPECT_TRUE(r.success()) << r.describe() << "\n" << r.outputTail;
    }

    // The process must end cleanly after the build ran on a thread that is gone
    TEST(NexusExit, buildOnWorkerThreadThenExit)
    {
        const process::Result r = runProbe("thread", TEST_NAME);
        EXPECT_EQ(r.termSignal, 0) << r.describe() << "\n" << r.outputTail;
        EXPECT_TRUE(r.success()) << r.describe() << "\n" << r.outputTail;
    }
} // namespace
