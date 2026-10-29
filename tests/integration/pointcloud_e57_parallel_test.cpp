/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// libE57Format shares state between the readers of a process: E57 files read at the same time (a
// dataset indexed in parallel, builds next to indexing) crash it. One E57 file goes through the
// reader at a time.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "mio.h"
#include "pointcloud.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"

namespace
{
    using namespace ddb;

    constexpr int Threads = 8;
    constexpr int ReadsPerThread = 4;

    fs::path e57Asset(TestArea &ta)
    {
        return ta.downloadTestAsset(testDataUrl("point-clouds/A4.e57"), "A4.e57");
    }

    TEST(PointCloudE57Parallel, InfoOfSeveralFilesAtOnce)
    {
        TestArea ta(TEST_NAME);
        const fs::path asset = e57Asset(ta);

        // A copy per thread: the files are distinct, as the files of a dataset are
        std::vector<std::string> files;
        for (int i = 0; i < Threads; i++)
        {
            const fs::path copy = ta.getPath("copy" + std::to_string(i) + ".e57");
            fs::copy_file(asset, copy, fs::copy_options::overwrite_existing);
            files.push_back(io::toUtf8(copy));
        }

        PointCloudInfo reference;
        ASSERT_TRUE(getPointCloudInfo(io::toUtf8(asset), reference));
        ASSERT_GT(reference.pointCount, 0u);

        std::atomic<int> failures{0};
        std::atomic<int> wrongCounts{0};
        std::vector<std::thread> threads;
        for (const auto &file : files)
        {
            threads.emplace_back(
                [&, file]()
                {
                    for (int n = 0; n < ReadsPerThread; n++)
                    {
                        try
                        {
                            PointCloudInfo info;
                            if (!getPointCloudInfo(file, info))
                                failures++;
                            else if (info.pointCount != reference.pointCount)
                                wrongCounts++;
                        }
                        catch (const std::exception &)
                        {
                            failures++;
                        }
                    }
                });
        }
        for (auto &t : threads)
            t.join();

        EXPECT_EQ(failures.load(), 0);
        EXPECT_EQ(wrongCounts.load(), 0);
    }

    TEST(PointCloudE57Parallel, TranslationsToLasAtOnce)
    {
        TestArea ta(TEST_NAME);
        const fs::path asset = e57Asset(ta);

        std::atomic<int> failures{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; i++)
        {
            const fs::path copy = ta.getPath("translate" + std::to_string(i) + ".e57");
            fs::copy_file(asset, copy, fs::copy_options::overwrite_existing);
            const fs::path las = ta.getPath("translate" + std::to_string(i) + ".las");
            threads.emplace_back(
                [&failures, copy, las]()
                {
                    try
                    {
                        translateToLas(io::toUtf8(copy), io::toUtf8(las));
                        if (!fs::exists(las))
                            failures++;
                    }
                    catch (const std::exception &)
                    {
                        failures++;
                    }
                });
        }
        for (auto &t : threads)
            t.join();

        EXPECT_EQ(failures.load(), 0);
    }
}
