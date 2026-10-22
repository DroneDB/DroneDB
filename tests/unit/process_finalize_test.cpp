/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Windows exit hang (CAbi.MaskBorders, CI run 39697846344): the test passed, then its process
// never exited. GDAL's thread pool (NUM_THREADS) was torn down by GDAL's DllMain after
// ExitProcess had terminated its workers, and waited for a worker that was terminated before it
// declared its job finished. The test binaries and the CLI now finalize GDAL at the end of main
// (ddb::finalizeProcess, process_init.h), which joins the workers while they still run.
//
// Windows only terminates threads that way, but the invariant is the same everywhere: once main
// has returned, no GDAL worker thread is left. A child process of this binary runs a threaded
// GDAL write and checks, from an exit-time handler (it runs after main returned), that the
// process has no more threads than before GDAL ran. Linux only: /proc/self/task lists the
// threads of the process exactly, while on Windows the loader's and the system thread pool's
// threads come and go.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "fs.h"
#include "gdal_raii.h"
#include "gtest/gtest.h"
#include "process_runner.h"
#include "mio.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    // Set by the parent test: the folder the child writes its rasters in
    constexpr const char *ChildFolderEnv = "DDB_TEST_FINALIZE_FOLDER";
    constexpr const char *ChildTest = "ProcessFinalize.DISABLED_childLeavesNoGdalWorkerAtExit";

#ifdef __linux__
    // The threads of this process that have not started to exit. A joined thread can still be
    // listed in /proc/self/task for a moment (the kernel removes it after pthread_join returned),
    // but it has PF_EXITING (0x4) in its flags (field 9 of its stat), set before the join returns.
    size_t threadCount()
    {
        constexpr unsigned long PfExiting = 0x4;
        size_t n = 0;
        for (const auto &task : fs::directory_iterator("/proc/self/task"))
        {
            std::ifstream stat(task.path() / "stat");
            std::string line;
            if (!std::getline(stat, line))
                continue; // gone meanwhile
            // The name (field 2) is in parentheses and may contain spaces: fields 3.. follow it
            std::istringstream fields(line.substr(line.rfind(')') + 1));
            std::string field;
            for (int i = 3; i < 9; i++)
                fields >> field;
            unsigned long flags = 0;
            if ((fields >> flags) && (flags & PfExiting) == 0)
                n++;
        }
        return n;
    }

    size_t threadsBeforeGdal = 0;

    // Runs once main has returned: exit code 3 when a thread GDAL started is still alive
    void checkNoGdalWorkerAtExit()
    {
        const size_t threads = threadCount();
        if (threads > threadsBeforeGdal)
        {
            std::fprintf(stderr, "%zu threads after main returned, %zu before GDAL ran: GDAL's workers are alive\n",
                         threads, threadsBeforeGdal);
            std::fflush(stderr);
            std::_Exit(3);
        }
    }
#endif

    // Child side, run in its own process by leavesNoGdalWorkerAtExit (disabled: never part of a
    // normal run)
    TEST(ProcessFinalize, DISABLED_childLeavesNoGdalWorkerAtExit)
    {
        const char *folder = std::getenv(ChildFolderEnv);
        if (folder == nullptr)
            GTEST_SKIP() << "Only run by ProcessFinalize.leavesNoGdalWorkerAtExit";
#ifdef __linux__
        // A thread started and joined first: a runtime that starts a thread of its own with the
        // first thread of the process (TSan's background thread) has it in the count already
        std::thread([] {}).join();
        threadsBeforeGdal = threadCount();

        // A tiled, compressed GeoTIFF with NUM_THREADS, like DDBMaskBorders writes: GDAL
        // compresses its blocks on the workers of its global thread pool
        testutils::GeoRasterSpec spec;
        spec.width = 1024;
        spec.height = 1024;
        spec.value = [](int band, int x, int y) { return static_cast<double>((x * 7 + y * 13 + band) % 251); };
        const fs::path input = testutils::createGeoRaster(fs::path(io::fromUtf8(folder)) / "input.tif", spec);
        const fs::path output = fs::path(io::fromUtf8(folder)) / "output.tif";
        {
            const auto source = adoptDataset(GDALOpen(io::toUtf8(input).c_str(), GA_ReadOnly));
            ASSERT_TRUE(source);
            CPLStringList options;
            options.AddString("TILED=YES");
            options.AddString("BLOCKXSIZE=256");
            options.AddString("BLOCKYSIZE=256");
            options.AddString("COMPRESS=DEFLATE");
            options.AddString("NUM_THREADS=4");
            const auto copy = adoptDataset(GDALCreateCopy(GDALGetDriverByName("GTiff"), io::toUtf8(output).c_str(),
                                                          datasetHandle(source), FALSE, options.List(), nullptr,
                                                          nullptr));
            ASSERT_TRUE(copy);
        }
        ASSERT_GT(threadCount(), threadsBeforeGdal) << "GDAL started no worker thread: nothing to check";

        // Registered after main's own handlers, so it runs before them
        ASSERT_EQ(std::atexit(&checkNoGdalWorkerAtExit), 0);
#endif
    }

    TEST(ProcessFinalize, leavesNoGdalWorkerAtExit)
    {
#ifndef __linux__
        GTEST_SKIP() << "Counts the threads of the child in /proc/self/task";
#else
        TestArea area(TEST_NAME, true);
        const fs::path self = io::getExeFolderPath() / DDB_UNIT_TESTS_NAME;
        process::Options options;
        options.envOverrides = {{ChildFolderEnv, io::toUtf8(area.getFolder())}};
        options.maxRuntime = std::chrono::minutes(2);
        const process::Result r = process::run(
            self, {"--gtest_also_run_disabled_tests", std::string("--gtest_filter=") + ChildTest}, options);
        // 3: a GDAL worker thread was alive after main returned
        EXPECT_TRUE(r.success()) << r.describe() << "\n" << r.tail(4000);
        EXPECT_NE(r.outputTail.find("[  PASSED  ] 1 test"), std::string::npos) << r.tail(4000);
#endif
    }
}
