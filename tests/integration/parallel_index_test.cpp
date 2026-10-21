/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDB-09: indexing hashes and parses files on several threads (parallel::forEachIndex), with
// results, errors and callbacks identical to the serial run.

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "databasepool.h"
#include "dbops.h"
#include "ddb.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "parallel.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    // Sets a config option for the scope, restores the previous value after
    class ScopedConfigOption
    {
    public:
        ScopedConfigOption(const char *name, const char *value) : name_(name)
        {
            const char *previous = CPLGetConfigOption(name, nullptr);
            hadValue_ = previous != nullptr;
            if (hadValue_)
                previous_ = previous;
            CPLSetConfigOption(name, value);
        }
        ~ScopedConfigOption()
        {
            CPLSetConfigOption(name_.c_str(), hadValue_ ? previous_.c_str() : nullptr);
        }

    private:
        std::string name_;
        std::string previous_;
        bool hadValue_ = false;
    };

    // ---- parallel::forEachIndex -----------------------------------------------------------

    TEST(DDB09_Parallel, visitsEveryIndexOnce)
    {
        constexpr size_t count = 1000;
        std::vector<std::atomic<int>> visits(count);
        parallel::forEachIndex(count, 8, [&](size_t i) { visits[i]++; });

        for (size_t i = 0; i < count; i++)
            EXPECT_EQ(visits[i].load(), 1) << i;
    }

    TEST(DDB09_Parallel, usesAtMostMaxThreads)
    {
        std::mutex mutex;
        std::set<std::thread::id> threads;
        std::atomic<int> running{0};
        std::atomic<int> peak{0};

        parallel::forEachIndex(64, 4, [&](size_t)
                               {
            const int now = ++running;
            int seen = peak.load();
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
            {
                std::lock_guard<std::mutex> lock(mutex);
                threads.insert(std::this_thread::get_id());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            --running; });

        EXPECT_LE(threads.size(), 4u);
        EXPECT_LE(peak.load(), 4);
        EXPECT_GT(peak.load(), 1) << "the work did not run in parallel";
    }

    TEST(DDB09_Parallel, oneThreadRunsInlineInOrder)
    {
        const auto caller = std::this_thread::get_id();
        std::vector<size_t> order;
        parallel::forEachIndex(10, 1, [&](size_t i)
                               {
            EXPECT_EQ(std::this_thread::get_id(), caller);
            order.push_back(i); });

        ASSERT_EQ(order.size(), 10u);
        for (size_t i = 0; i < order.size(); i++)
            EXPECT_EQ(order[i], i);

        // Zero threads and zero items are valid too
        size_t calls = 0;
        parallel::forEachIndex(3, 0, [&](size_t) { calls++; });
        parallel::forEachIndex(0, 8, [&](size_t) { calls++; });
        EXPECT_EQ(calls, 3u);
    }

    TEST(DDB09_Parallel, rethrowsTheFirstFailureInIndexOrder)
    {
        // Index 7 fails at once, index 3 fails later: a serial loop would report 3
        std::vector<std::atomic<int>> ran(32);
        try
        {
            parallel::forEachIndex(32, 8, [&](size_t i)
                                   {
                ran[i]++;
                if (i == 3)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    throw std::runtime_error("3");
                }
                if (i == 7)
                    throw std::runtime_error("7"); });
            FAIL() << "no exception";
        }
        catch (const std::runtime_error &e)
        {
            EXPECT_STREQ(e.what(), "3");
        }

        for (size_t i = 0; i <= 3; i++)
            EXPECT_EQ(ran[i].load(), 1) << "index " << i << " must run: it comes before the failure";
    }

    // PDAL replaces GDAL's process-wide error handler while one of its stages runs, and that
    // handler writes into the running stage's log without synchronization: a GDAL error raised
    // by an indexing thread at that moment raced with the PDAL thread (TSan, DQ-01). The calls
    // run by forEachIndex, on every thread, must not reach the process-wide handler; the error
    // itself is still recorded for the thread that raised it.
    std::atomic<int> processWideGdalErrors{0};
    void CPL_STDCALL countProcessWideGdalError(CPLErr, CPLErrorNum, const char *) { processWideGdalErrors++; }

    TEST(DDB09_Parallel, gdalErrorsBypassTheProcessWideHandler)
    {
        processWideGdalErrors = 0;
        const CPLErrorHandler previous = CPLSetErrorHandler(&countProcessWideGdalError);

        constexpr size_t count = 16;
        std::atomic<int> recorded{0};
        for (const size_t threads : {size_t{1}, size_t{8}})
            parallel::forEachIndex(count, threads, [&](size_t i)
                                   {
                                       CPLError(CE_Failure, CPLE_AppDefined, "DDB09 expected error %d",
                                                static_cast<int>(i));
                                       if (CPLGetLastErrorType() == CE_Failure)
                                           recorded++;
                                   });
        const int fromParallel = processWideGdalErrors.load();

        // The handler is back once forEachIndex returns
        CPLError(CE_Failure, CPLE_AppDefined, "DDB09 expected error after forEachIndex");
        const int afterwards = processWideGdalErrors.load();
        CPLSetErrorHandler(previous);
        CPLErrorReset();

        EXPECT_EQ(fromParallel, 0);
        EXPECT_EQ(recorded.load(), static_cast<int>(2 * count));
        EXPECT_EQ(afterwards, 1);
    }

    TEST(DDB09_Parallel, threadsOption)
    {
        const int cores = static_cast<int>(parallel::hardwareThreads());
        {
            ScopedConfigOption opt("DDB_TEST_THREADS", nullptr);
            EXPECT_EQ(parallel::threadsOption("DDB_TEST_THREADS", 8), std::min(8, cores));
        }
        {
            ScopedConfigOption opt("DDB_TEST_THREADS", "3");
            EXPECT_EQ(parallel::threadsOption("DDB_TEST_THREADS", 8), 3);
        }
        {
            ScopedConfigOption opt("DDB_TEST_THREADS", "0");
            EXPECT_EQ(parallel::threadsOption("DDB_TEST_THREADS", 8), 0);
        }
        for (const char *invalid : {"-1", "abc", "4x", ""})
        {
            ScopedConfigOption opt("DDB_TEST_THREADS", invalid);
            EXPECT_EQ(parallel::threadsOption("DDB_TEST_THREADS", 8), std::min(8, cores)) << invalid;
        }
    }

    // ---- Indexing ---------------------------------------------------------------------------

    // A folder with every kind of file the parser handles differently
    fs::path makeMixedFolder(TestArea &ta)
    {
        const std::string base = "https://github.com/DroneDB/test_data/raw/refs/heads/master/";
        const auto root = ta.getFolder("ds");
        for (const auto &[url, name] : std::vector<std::pair<std::string, std::string>>{
                 {"images/DJI_0018.JPG", "DJI_0018.JPG"},
                 {"images/DJI_0022.JPG", "DJI_0022.JPG"},
                 {"images/DJI_0032.JPG", "sub/DJI_0032.JPG"},
                 {"ortho/wro.tif", "wro.tif"},
                 {"brighton/point_cloud.laz", "sub/deeper/point_cloud.laz"},
                 {"vector/test.geojson", "test.geojson"}})
        {
            const auto file = ta.downloadTestAsset(base + url, fs::path(url).filename().string());
            fs::create_directories((root / name).parent_path());
            if (!fs::exists(root / name))
                fs::copy_file(file, root / name);
        }
        fileWriteAllText(root / "notes.txt", "hello");
        fileWriteAllText(root / "broken.tif", "not a tiff");
        return root;
    }

    // Every column of every entry, in path order
    std::vector<std::string> dumpEntries(Database *db)
    {
        std::vector<std::string> rows;
        auto q = db->query("SELECT path, hash, type, properties, mtime, size, depth, "
                           "AsText(point_geom), AsText(polygon_geom) FROM entries ORDER BY path");
        while (q->fetch())
        {
            std::string row;
            for (int c = 0; c < 9; c++)
                row += q->getText(c) + "|";
            rows.push_back(row);
        }
        return rows;
    }

    struct IndexRun
    {
        std::vector<std::string> rows;
        std::vector<std::string> results; // path:updated, in AddResult order
        std::vector<std::thread::id> callbackThreads;
    };

    IndexRun indexWith(const fs::path &root, const char *threads)
    {
        ScopedConfigOption opt("DDB_INDEX_THREADS", threads);
        // The previous run left its connection idle: Windows cannot delete an open file
        DatabasePool::instance().releaseConnections(root);
        io::assureIsRemoved(root / DDB_FOLDER);
        initIndex(root.string());
        auto db = ddb::open(root.string(), true);

        IndexRun run;
        AddOptions options;
        options.stopOnError = false;
        AddResult result;
        addToIndexEx(db.get(), {root.string()}, options, result, [&run](const Entry &, bool)
                     {
            run.callbackThreads.push_back(std::this_thread::get_id());
            return true; });

        EXPECT_TRUE(result.errors.empty());
        for (const auto &[entry, updated] : result.entries)
            run.results.push_back(entry.path + ":" + std::to_string(updated));
        run.rows = dumpEntries(db.get());
        return run;
    }

    TEST(DDB09_Index, parallelMatchesSerial)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = makeMixedFolder(ta);

        const auto serial = indexWith(root, "1");
        const auto parallel = indexWith(root, "8");

        ASSERT_EQ(serial.rows.size(), 10u) << "8 files + 2 folders";
        EXPECT_EQ(parallel.rows, serial.rows);
        EXPECT_EQ(parallel.results, serial.results);
    }

    TEST(DDB09_Index, callbackRunsOnTheCallerThread)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = makeMixedFolder(ta);

        const auto run = indexWith(root, "8");
        ASSERT_FALSE(run.callbackThreads.empty());
        for (const auto &id : run.callbackThreads)
            EXPECT_EQ(id, std::this_thread::get_id());
    }

#ifndef _WIN32
    // An unreadable file fails only itself (stopOnError=false), and the first one in input
    // order aborts the batch (stopOnError=true), with nothing committed
    TEST(DDB09_Index, errorsAreCollectedNotLost)
    {
        if (geteuid() == 0)
            GTEST_SKIP() << "root reads files without read permission";

        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        std::vector<std::string> paths;
        for (int i = 0; i < 40; i++)
        {
            const auto file = root / ("f" + std::to_string(100 + i) + ".txt");
            fileWriteAllText(file, "content " + std::to_string(i));
            paths.push_back(file.string());
        }
        const std::set<int> unreadable = {105, 117, 133};
        for (int i : unreadable)
            fs::permissions(root / ("f" + std::to_string(i) + ".txt"), fs::perms::none);

        ScopedConfigOption opt("DDB_INDEX_THREADS", "8");
        initIndex(root.string());
        auto db = ddb::open(root.string(), true);

        AddOptions options;
        options.stopOnError = false;
        AddResult result;
        addToIndexEx(db.get(), paths, options, result);

        ASSERT_EQ(result.errors.size(), unreadable.size());
        auto it = unreadable.begin();
        for (const auto &error : result.errors)
        {
            EXPECT_EQ(error.path, "f" + std::to_string(*it++) + ".txt");
            EXPECT_EQ(error.code, "FS");
        }
        EXPECT_EQ(result.entries.size(), paths.size() - unreadable.size());

        // stopOnError: the batch fails on f105 and commits nothing
        io::assureIsRemoved(root / DDB_FOLDER);
        initIndex(root.string());
        db = ddb::open(root.string(), true);
        AddResult aborted;
        try
        {
            addToIndexEx(db.get(), paths, AddOptions{}, aborted);
            FAIL() << "no exception";
        }
        catch (const FSException &e)
        {
            EXPECT_NE(std::string(e.what()).find("f105.txt"), std::string::npos) << e.what();
        }
        auto q = db->query("SELECT COUNT(*) FROM entries");
        q->fetch();
        EXPECT_EQ(q->getInt(0), 0);

        for (int i : unreadable)
            fs::permissions(root / ("f" + std::to_string(i) + ".txt"), fs::perms::owner_all);
    }
#endif

    // Many threads parsing the same files give the same entries as one thread (Exiv2, GDAL,
    // PDAL and OGR state is per call)
    TEST(DDB09_Index, concurrentParsesMatchSerial)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = makeMixedFolder(ta);
        const std::vector<fs::path> files = {root / "DJI_0018.JPG", root / "DJI_0022.JPG", root / "sub/DJI_0032.JPG",
                                             root / "wro.tif", root / "sub/deeper/point_cloud.laz",
                                             root / "test.geojson", root / "broken.tif"};

        const auto parse = [&](const fs::path &file)
        {
            Entry e;
            parseEntry(file, root, e, true);
            json j;
            e.toJSON(j);
            return j.dump();
        };

        std::vector<std::string> expected;
        for (const auto &f : files)
            expected.push_back(parse(f));

        constexpr size_t rounds = 8;
        std::vector<std::string> actual(files.size() * rounds);
        parallel::forEachIndex(actual.size(), 16, [&](size_t i)
                               { actual[i] = parse(files[i % files.size()]); });

        for (size_t i = 0; i < actual.size(); i++)
            EXPECT_EQ(actual[i], expected[i % files.size()]) << files[i % files.size()];
    }

} // namespace
