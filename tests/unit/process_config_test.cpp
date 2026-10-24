/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-15 (XI-06): the process configuration (ProcessConfig: DDBConfigure > environment >
// default) and the process-wide CPU token pool (parallel.h). The pool is only ever tried: these
// tests prove that nested sections, concurrent calls and a pool of 0 never wait for a token,
// with latches and barriers instead of timings (a broken pool hangs a test, which the ctest
// timeout reports, rather than passing by luck).

#include <algorithm>
#include <atomic>
#include <barrier>
#include <latch>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ddb.h"
#include "exceptions.h"
#include "gdal_raii.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "parallel.h"
#include "process_config.h"
#include "test.h"
#include "testarea.h"
#include "userprofile.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    // The largest value `current` takes while it is recorded
    class MaxRecorder
    {
        std::atomic<size_t> max_{0};

    public:
        void record(size_t current)
        {
            size_t seen = max_.load();
            while (current > seen && !max_.compare_exchange_weak(seen, current))
            {
            }
        }
        size_t max() const { return max_.load(); }
    };

    // ---- ProcessConfig ------------------------------------------------------------------

    TEST(ProcessConfig, PrecedenceConfigureEnvironmentDefault)
    {
        EnvVarGuard io("DDB_IO_THREADS");
        unsetEnv("DDB_IO_THREADS");
        auto &config = ProcessConfig::get();
        EXPECT_EQ(config.ioThreads(), 4u);
        EXPECT_EQ(config.effective()["sources"]["ioThreads"], "default");

        setEnv("DDB_IO_THREADS", "3");
        EXPECT_EQ(config.ioThreads(), 3u);
        EXPECT_EQ(config.effective()["sources"]["ioThreads"], "environment");

        {
            const ScopedProcessConfig configured(R"({"ioThreads": 7})");
            EXPECT_EQ(config.ioThreads(), 7u);
            EXPECT_EQ(config.effective()["sources"]["ioThreads"], "configure");
        }
        // null (ScopedProcessConfig's reset) gives the environment variable back
        EXPECT_EQ(config.ioThreads(), 3u);

        // An invalid environment value is ignored
        for (const char *invalid : {"0", "-2", "abc", "4x", " 4", "257", "99999999999999999999999"})
        {
            setEnv("DDB_IO_THREADS", invalid);
            EXPECT_EQ(config.ioThreads(), 4u) << invalid;
        }
    }

    TEST(ProcessConfig, Defaults)
    {
        EnvVarGuard pool("DDB_CPU_POOL_SIZE");
        EnvVarGuard floor("DDB_MIN_FREE_DISK_MB");
        unsetEnv("DDB_CPU_POOL_SIZE");
        unsetEnv("DDB_MIN_FREE_DISK_MB");
        auto &config = ProcessConfig::get();
        EXPECT_EQ(config.cpuPoolSize(), parallel::hardwareThreads() - 1);
        EXPECT_EQ(config.minFreeDiskMb(), 512u);
        EXPECT_EQ(config.cacheDir(), UserProfile::defaultProfileDir());

        const json effective = config.effective();
        EXPECT_EQ(effective["hardwareThreads"], parallel::hardwareThreads());
        EXPECT_EQ(effective["settings"]["cpuPoolSize"], parallel::hardwareThreads() - 1);
        EXPECT_EQ(effective["sources"]["cpuPoolSize"], "default");
        EXPECT_EQ(effective["sources"]["minFreeDiskMb"], "default");
    }

    TEST(ProcessConfig, InvalidChangesChangeNothing)
    {
        auto &config = ProcessConfig::get();
        const json before = config.effective();
        const std::vector<std::string> invalid = {
            R"([])",
            R"("cpuPoolSize")",
            R"({"bogus": 1})",
            R"({"ioThreads": 2, "bogus": 1})",  // the valid member is not applied either
            R"({"ioThreads": 0})",
            R"({"ioThreads": 257})",
            R"({"ioThreads": -1})",
            R"({"ioThreads": 2.0})",
            R"({"ioThreads": "2"})",
            R"({"ioThreads": true})",
            R"({"cpuPoolSize": 4097})",
            R"({"minFreeDiskMb": 10000000000001})",
            R"({"gdalCacheMb": 15})",
            R"({"gdalCacheMb": 1048577})",
            R"({"gdalCacheMb": 64.5})",
            R"({"gdalCacheMb": "64"})",
            R"({"cacheDir": ""})",
            R"({"cacheDir": "relative/folder"})",
            R"({"cacheDir": 5})",
        };
        for (const auto &changes : invalid)
        {
            EXPECT_THROW(config.configure(json::parse(changes)), InvalidArgsException) << changes;
            EXPECT_EQ(config.effective(), before) << changes;
        }
    }

    TEST(ProcessConfig, LimitsAreAccepted)
    {
        const ScopedProcessConfig configured(
            R"({"cpuPoolSize": 0, "ioThreads": 256, "minFreeDiskMb": 10000000000000, "gdalCacheMb": 1048576})");
        auto &config = ProcessConfig::get();
        EXPECT_EQ(config.cpuPoolSize(), 0u);
        EXPECT_EQ(config.ioThreads(), 256u);
        EXPECT_EQ(config.minFreeDiskMb(), 10000000000000u);
        EXPECT_EQ(config.gdalCacheMb(), 1048576u);
        ProcessConfig::get().configure(json::parse(R"({"gdalCacheMb": 16})"));
        EXPECT_EQ(config.gdalCacheMb(), 16u);
    }

    // cacheDir moves the caches, not the profile (credentials)
    TEST(ProcessConfig, CacheDirHoldsTheCaches)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path cache = ta.getPath("cache/nested"); // created when first needed
        const json changes = {{"cacheDir", io::toUtf8(cache)}};
        const ScopedProcessConfig configured(changes.dump());

        auto *profile = UserProfile::get();
        EXPECT_EQ(ProcessConfig::get().cacheDir(), cache);
        EXPECT_EQ(profile->getCacheDir(), cache);
        EXPECT_TRUE(fs::is_directory(cache));
        EXPECT_EQ(profile->getThumbsDir(), cache / "thumbs");
        EXPECT_EQ(profile->getTilesDir(), cache / "tiles");
        EXPECT_EQ(profile->getTemplatesDir(), cache / "templates");
        EXPECT_EQ(profile->getCachePath("dsm_service_cache"), cache / "dsm_service_cache");
        EXPECT_TRUE(fs::is_directory(cache / "tiles"));
        EXPECT_EQ(profile->getAuthFile(), UserProfile::defaultProfileDir() / "auth.json");
    }

    // ---- The CPU token pool -------------------------------------------------------------

    TEST(CpuPool, TokensAreBoundedAndGivenBack)
    {
        const ScopedProcessConfig configured(R"({"cpuPoolSize": 3})");
        ASSERT_EQ(parallel::cpuTokensInUse(), 0u);
        {
            auto a = parallel::tryAcquireThreads(10);
            EXPECT_EQ(a.count(), 3u);
            EXPECT_EQ(a.threads(), 4u);
            EXPECT_EQ(parallel::gdalNumThreads(a), "NUM_THREADS=4");

            auto b = parallel::tryAcquireThreads(10);
            EXPECT_EQ(b.count(), 0u); // never waits: no token is free
            EXPECT_EQ(b.threads(), 1u);
            EXPECT_EQ(parallel::gdalNumThreads(b), "NUM_THREADS=1");

            a.release(1);
            EXPECT_EQ(parallel::cpuTokensInUse(), 2u);
            EXPECT_EQ(b.tryAcquire(5), 1u);

            parallel::CpuTokens moved = std::move(a);
            EXPECT_EQ(moved.count(), 2u);
            EXPECT_EQ(a.count(), 0u); // NOLINT(bugprone-use-after-move): moved-from is empty
            EXPECT_EQ(parallel::cpuTokensInUse(), 3u);
        }
        EXPECT_EQ(parallel::cpuTokensInUse(), 0u);

        // A section of one thread asks for nothing
        EXPECT_EQ(parallel::tryAcquireThreads(1).count(), 0u);
        EXPECT_EQ(parallel::tryAcquireThreads(0).count(), 0u);
    }

    TEST(CpuPool, ShrinkingNeverTakesTokensBack)
    {
        const ScopedProcessConfig configured(R"({"cpuPoolSize": 4})");
        auto held = parallel::tryAcquireThreads(5);
        ASSERT_EQ(held.count(), 4u);

        const ScopedProcessConfig smaller(R"({"cpuPoolSize": 1})");
        EXPECT_EQ(held.count(), 4u);
        held.release(2); // 2 in use, above the new size
        EXPECT_EQ(parallel::tryAcquireThreads(2).count(), 0u);
        held.release(2);
        EXPECT_EQ(parallel::tryAcquireThreads(5).count(), 1u);
        EXPECT_EQ(parallel::cpuTokensInUse(), 0u);
    }

    // P3-7 (M4): a pool of 1, sections nested in a section's worker and GDAL threads in a
    // worker. Every section gets 0..N tokens and none waits, so the whole tree completes and
    // never holds more than the pool.
    TEST(CpuPool, PoolOfOneNestedSectionsAndGdalComplete)
    {
        const ScopedProcessConfig configured(R"({"cpuPoolSize": 1})");
        MaxRecorder inUse;
        std::atomic<size_t> innerCalls{0};
        std::atomic<size_t> gdalWrites{0};
        std::mutex threadsMutex;
        std::set<std::thread::id> outerThreads;

        parallel::forEachIndex(6, 4, [&](size_t i) {
            {
                std::lock_guard<std::mutex> lock(threadsMutex);
                outerThreads.insert(std::this_thread::get_id());
            }
            inUse.record(parallel::cpuTokensInUse());
            parallel::forEachIndex(5, 4, [&](size_t) {
                inUse.record(parallel::cpuTokensInUse());
                innerCalls++;
            });

            // GDAL compression threads from the pool, in a worker as in a build
            const auto cpu = parallel::tryAcquireThreads(parallel::hardwareThreads());
            inUse.record(parallel::cpuTokensInUse());
            const GDALDatasetUniquePtr src = adoptDataset(
                GDALCreate(GDALGetDriverByName("MEM"), "", 64, 64, 1, GDT_Byte, nullptr));
            ASSERT_TRUE(src);
            const std::string numThreads = parallel::gdalNumThreads(cpu);
            const char *options[] = {"COMPRESS=DEFLATE", "TILED=YES", numThreads.c_str(), nullptr};
            const std::string path = "/vsimem/cpu_pool_" + std::to_string(i) + ".tif";
            {
                const GDALDatasetUniquePtr out = adoptDataset(GDALCreateCopy(
                    GDALGetDriverByName("GTiff"), path.c_str(), datasetHandle(src), FALSE,
                    const_cast<char **>(options), nullptr, nullptr));
                ASSERT_TRUE(out);
            }
            VSIUnlink(path.c_str());
            gdalWrites++;
        });

        EXPECT_EQ(innerCalls.load(), 30u);
        EXPECT_EQ(gdalWrites.load(), 6u);
        EXPECT_LE(inUse.max(), 1u);
        // The calling thread and at most one worker (the pool's only token) ran the outer items
        EXPECT_LE(outerThreads.size(), 2u);
        EXPECT_EQ(parallel::cpuTokensInUse(), 0u);
    }

    // P3-7 (M4): with a pool of 0, two calls at once both run, each on its own calling
    // thread. The barrier needs both calls inside their first item: a call that waited for a
    // token would never get there.
    TEST(CpuPool, PoolOfZeroConcurrentCallsComplete)
    {
        const ScopedProcessConfig configured(R"({"cpuPoolSize": 0})");
        std::barrier bothInside(2);

        const auto call = [&](std::set<std::thread::id> &threads) {
            std::mutex mutex;
            parallel::forEachIndex(8, 8, [&](size_t i) {
                if (i == 0)
                    bothInside.arrive_and_wait();
                std::lock_guard<std::mutex> lock(mutex);
                threads.insert(std::this_thread::get_id());
            });
        };

        std::set<std::thread::id> threadsA, threadsB;
        std::thread::id callerA, callerB;
        std::thread a([&] { callerA = std::this_thread::get_id(); call(threadsA); });
        std::thread b([&] { callerB = std::this_thread::get_id(); call(threadsB); });
        a.join();
        b.join();

        EXPECT_EQ(threadsA, std::set<std::thread::id>{callerA});
        EXPECT_EQ(threadsB, std::set<std::thread::id>{callerB});
        EXPECT_EQ(parallel::cpuTokensInUse(), 0u);
    }

    // A section that started without tokens takes the ones another section gives back: the
    // calling thread tries again after each of its items. Item 1 waits for item 2, which only
    // a second thread can run while item 1 blocks the calling thread (or the other way round).
    TEST(CpuPool, SectionGrowsWhenTokensComeBack)
    {
        const ScopedProcessConfig configured(R"({"cpuPoolSize": 2})");
        auto other = parallel::tryAcquireThreads(3); // another section holds the whole pool
        ASSERT_EQ(other.count(), 2u);

        std::latch item2Done(1);
        std::atomic<size_t> calls{0};
        parallel::forEachIndex(4, 3, [&](size_t i) {
            if (i == 0)
                other.release(2); // the other section ends
            if (i == 1)
                item2Done.wait();
            if (i == 2)
                item2Done.count_down();
            calls++;
        });

        EXPECT_EQ(calls.load(), 4u);
        EXPECT_EQ(parallel::cpuTokensInUse(), 0u);
    }

    TEST(CpuPool, TokensAreGivenBackWhenAnItemThrows)
    {
        const ScopedProcessConfig configured(R"({"cpuPoolSize": 3})");
        EXPECT_THROW(parallel::forEachIndex(16, 4,
                                            [](size_t i) {
                                                if (i == 5)
                                                    throw std::runtime_error("item 5");
                                            }),
                     std::runtime_error);
        EXPECT_EQ(parallel::cpuTokensInUse(), 0u);
    }

    // Disk-bound sections run ioThreads threads per call and take no CPU token: three items
    // meet at a barrier of 3 with a pool of 0.
    TEST(CpuPool, IoSectionsUseIoThreadsWithoutTokens)
    {
        const ScopedProcessConfig configured(R"({"cpuPoolSize": 0, "ioThreads": 3})");
        std::barrier threeInside(3);
        std::atomic<size_t> calls{0};
        MaxRecorder inUse;
        parallel::forEachIndexIo(9, [&](size_t i) {
            if (i < 3)
                threeInside.arrive_and_wait();
            inUse.record(parallel::cpuTokensInUse());
            calls++;
        });
        EXPECT_EQ(calls.load(), 9u);
        EXPECT_EQ(inUse.max(), 0u);
    }

    // DDBConfigure runs concurrently with the readers of the configuration and with parallel
    // sections (TSan: no data race); every call succeeds and the pool ends empty.
    TEST(CpuPool, ConfigureConcurrentlyWithCalls)
    {
        const ScopedProcessConfig configured(R"({"cpuPoolSize": 2, "ioThreads": 4})");
        constexpr int Rounds = 50;
        std::latch start(3);
        std::atomic<int> failures{0};

        std::thread configurer([&] {
            start.arrive_and_wait();
            for (int r = 0; r < Rounds; r++)
            {
                const std::string changes = "{\"cpuPoolSize\": " + std::to_string(r % 4) + ", \"ioThreads\": " +
                                            std::to_string(1 + r % 3) + "}";
                if (DDBConfigure(changes.c_str()) != DDBERR_NONE)
                    failures++;
            }
        });
        std::thread reader([&] {
            start.arrive_and_wait();
            for (int r = 0; r < Rounds; r++)
            {
                char *out = nullptr;
                if (DDBGetEffectiveConfig(&out) != DDBERR_NONE)
                    failures++;
                DDBFree(out);
            }
        });
        std::thread worker([&] {
            start.arrive_and_wait();
            for (int r = 0; r < Rounds; r++)
            {
                std::atomic<size_t> calls{0};
                parallel::forEachIndex(8, 4, [&](size_t) { calls++; });
                parallel::forEachIndexIo(4, [&](size_t) { calls++; });
                if (calls.load() != 12)
                    failures++;
            }
        });
        configurer.join();
        reader.join();
        worker.join();

        EXPECT_EQ(failures.load(), 0);
        EXPECT_EQ(parallel::cpuTokensInUse(), 0u);
    }

} // namespace
