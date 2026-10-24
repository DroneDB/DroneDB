/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Upstream, freeing a SpatiaLite connection cache (spatialite_cleanup_ex) also runs xmlCleanupParser(),
// the process-wide libxml2 teardown, which is not thread-safe: connections closed at the same
// time tore libxml2 down together. A teardown destroys libxml2's global mutexes (catalog,
// schema types) right after releasing them: a thread it had just woken then fails to unlock
// the destroyed mutex, and the next waiter sleeps forever. That was the rare 300 s hang of
// concurrentAdd.manyThreadsOneDatabaseBaseline and DDB02_Open.concurrentOpensAndReadsSucceed
// under a parallel ctest run, whose first closes run on many threads at once. Each teardown
// also dropped the per-thread libxml2 state of live threads, which LSan reports as leaks.
// vcpkg-overlays/libspatialite removes the call; freeSpatialiteCache serializes the frees.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "database.h"
#include "exceptions.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    // Rounds of: use libxml2 again, which arms one more teardown, then close several
    // connections at the same instant. The XML function validates against a schema: that
    // initializes libxml2's schema types, so each teardown holds the schema types mutex long
    // enough for the other closes to queue on it.
    void closeConnectionsTogether(const fs::path &file, const fs::path &schema, int rounds, int connections)
    {
        for (int r = 0; r < rounds; r++)
        {
            std::vector<std::unique_ptr<Database>> dbs;
            for (int i = 0; i < connections; i++)
            {
                dbs.push_back(std::make_unique<Database>());
                dbs.back()->open(file.string());
            }

            {
                auto q = dbs.front()->query("SELECT XB_Create(CAST('<a/>' AS BLOB), 1, ?) IS NOT NULL");
                q->bind(1, schema.string());
                if (!q->fetch() || q->getInt(0) != 1)
                    throw DBException("XB_Create did not validate the document against " + schema.string());
            }

            std::atomic<int> ready{0};
            std::vector<std::thread> closers;
            for (int i = 0; i < connections; i++)
                closers.emplace_back([&, i] {
                    ++ready;
                    while (ready.load() < connections)
                        std::this_thread::yield();
                    dbs[i]->close();
                });
            for (auto &t : closers)
                t.join();
        }
    }

    TEST(SpatialiteCache, concurrentClosesAfterLibxml2Use)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path file = ta.getFolder() / "index.sqlite";
        const fs::path schema = ta.getFolder() / "a.xsd";
        std::ofstream(schema.string())
            << R"(<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"><xs:element name="a"/></xs:schema>)";
        {
            Database db;
            db.open(file.string());
        }

        // The closes run on a thread of their own: a deadlocked round must fail this test,
        // not hang the test binary until the ctest timeout.
        struct Outcome
        {
            std::mutex mutex;
            std::condition_variable finished;
            bool done = false;
            std::string error;
        };
        auto outcome = std::make_shared<Outcome>();
        std::thread([outcome, file, schema] {
            std::string error;
            try
            {
                closeConnectionsTogether(file, schema, 200, 8);
            }
            catch (const std::exception &e)
            {
                error = e.what();
            }
            std::lock_guard<std::mutex> lock(outcome->mutex);
            outcome->error = error;
            outcome->done = true;
            outcome->finished.notify_all();
        }).detach();

        std::unique_lock<std::mutex> lock(outcome->mutex);
        ASSERT_TRUE(outcome->finished.wait_for(lock, std::chrono::seconds(120), [&] { return outcome->done; }))
            << "Concurrent SpatiaLite cache cleanups deadlocked";
        EXPECT_EQ(outcome->error, "");
    }
}
