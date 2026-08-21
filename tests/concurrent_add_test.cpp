/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Baseline / regression harness for the "database is locked" incident under concurrent DDBAdd
// calls to the same dataset.
//
// Phases 1-3 (transaction RAII, plan/compute/commit split, retry policy) are implemented, so this
// is now a real correctness gate: concurrent DDBAdd calls to the same dataset must not surface
// SQLITE_BUSY/"database is locked".

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <numeric>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "ddb.h"
#include "gtest/gtest.h"
#include "json.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;
using Clock = std::chrono::steady_clock;

// Writes `sizeBytes` of pseudo-random content so each file hashes differently and hashing cost is
// representative of real (non-trivially-compressible) payloads.
void writeRandomFile(const fs::path &path, size_t sizeBytes, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<char> buf(sizeBytes);
    for (auto &b : buf) b = static_cast<char>(rng());
    std::ofstream f(path.string(), std::ios::binary);
    f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
}

double percentile(std::vector<double> sorted, double p) {
    if (sorted.empty()) return 0.0;
    const size_t idx = std::min(sorted.size() - 1,
                                 static_cast<size_t>(p * static_cast<double>(sorted.size())));
    return sorted[idx];
}

// Baseline concurrent-write harness.
TEST(concurrentAdd, manyThreadsOneDatabaseBaseline) {
    TestArea ta(TEST_NAME, true);
    const auto root = ta.getFolder("ds");

    char *outPath = nullptr;
    ASSERT_EQ(DDBInit(root.string().c_str(), &outPath), DDBERR_NONE);
    DDBFree(outPath);

    constexpr int T = 16;             // concurrent writers, matches Hub UI parallelUploads-ish load
    constexpr int K = 25;              // files per writer
    constexpr size_t FILE_SIZE = 128 * 1024; // 128 KiB: observable hashing cost without 800 MiB disk I/O

    std::vector<std::vector<fs::path>> paths(T);
    for (int t = 0; t < T; ++t) {
        for (int k = 0; k < K; ++k) {
            auto p = root / ("file_" + std::to_string(t) + "_" + std::to_string(k) + ".bin");
            writeRandomFile(p, FILE_SIZE, static_cast<unsigned>(t * 1000 + k));
            paths[t].push_back(p);
        }
    }

    std::atomic<int> failures{0};
    std::vector<std::vector<double>> latenciesMs(T);
    std::vector<std::thread> threads;

    const auto wallStart = Clock::now();

    for (int t = 0; t < T; ++t) {
        threads.emplace_back([&, t] {
            for (int k = 0; k < K; ++k) {
                char *out = nullptr;
                const std::string pathStr = paths[t][k].string();
                const char *p[] = {pathStr.c_str()};
                const auto callStart = Clock::now();
                const auto err = DDBAdd(root.string().c_str(), p, 1, &out, false);
                const auto callEnd = Clock::now();
                latenciesMs[t].push_back(
                    std::chrono::duration<double, std::milli>(callEnd - callStart).count());
                if (err != DDBERR_NONE) {
                    ++failures;
                    fprintf(stderr, "[baseline] DDBAdd failed: %s\n", DDBGetLastError());
                }
                DDBFree(out);
            }
        });
    }
    for (auto &th : threads) th.join();

    const auto wallEnd = Clock::now();
    const double wallMs = std::chrono::duration<double, std::milli>(wallEnd - wallStart).count();

    std::vector<double> all;
    for (auto &v : latenciesMs)
        all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());

    fprintf(stderr,
            "[baseline] threads=%d filesPerThread=%d totalFiles=%d fileSize=%zuB "
            "wallClockMs=%.1f failures=%d "
            "perCallLatencyMs(p50=%.1f p95=%.1f p99=%.1f max=%.1f)\n",
            T, K, T * K, FILE_SIZE, wallMs, failures.load(),
            percentile(all, 0.50), percentile(all, 0.95), percentile(all, 0.99),
            all.empty() ? 0.0 : all.back());

    EXPECT_EQ(failures.load(), 0);
}

// Writes `sizeBytes` of deterministic pseudo-random ASCII digits so every writer parses the exact
// same bytes for a given seed (stable SHA256 and mtime) while the content stays plain text that
// DroneDB fingerprints as a generic, indexable entry.
void writeRandomTextFile(const fs::path &path, size_t sizeBytes, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<char> buf(sizeBytes, '0');
    for (auto &b : buf) b = static_cast<char>('0' + static_cast<int>(rng() % 10));
    std::ofstream f(path.string(), std::ios::binary);
    f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
}

// Regression for the commitAddEntries() completeness-contract gap. When a planned INSERT
// re-checks under the COMMIT transaction and finds
// that a concurrent writer already committed an identical row (same path, same mtime AND same
// hash), that path used to hit a bare `continue` and be reported in NONE of
// entries/unchanged/errors. The caller then saw entries+unchanged+errors < inputPaths and -- on
// the Registry side -- quarantined an already-indexed file, which the reconciliation sweep
// re-indexed, looping.
//
// The race window cannot be forced deterministically: COMMIT holds an IMMEDIATE transaction, so a
// second connection cannot write from inside the add callback. This stress shape is the only
// viable coverage: several threads fire DDBAddWithOptions on the SAME set of identical files in
// the same wave, and every successful call must uphold
// entries.size() + unchanged.size() + errors.size() == inputPaths.size().
//
// Before the fix this fails (a losing writer reports one or more vanished paths); after the fix
// the concurrent-identical path is routed into `unchanged`. Run with --gtest_repeat to raise the
// hit rate (each repeat gets a fresh database).
TEST(concurrentAdd, completenessUnderConcurrentIdenticalAdds) {
    TestArea ta(TEST_NAME, true);
    const auto root = ta.getFolder("ds");

    char *outPath = nullptr;
    ASSERT_EQ(DDBInit(root.string().c_str(), &outPath), DDBERR_NONE);
    DDBFree(outPath);

    const int T = 8;  // concurrent writers racing the same files
    const int N = 20; // identical files; every writer adds this same set of N paths

    std::vector<fs::path> files(N);
    std::vector<std::string> pathStrs(N);
    std::vector<const char *> pathPtrs(N);
    for (int i = 0; i < N; ++i) {
        files[i] = root / ("shared_" + std::to_string(i) + ".txt");
        writeRandomTextFile(files[i], 256 * 1024, static_cast<unsigned>(i));
        pathStrs[i] = files[i].string();
        pathPtrs[i] = pathStrs[i].c_str();
    }

    std::atomic<int> violations{0}; // calls whose buckets did not sum to N (the contract gap)
    std::atomic<int> duplicates{0}; // a path reported in more than one bucket
    std::atomic<int> busy{0};
    std::atomic<int> otherErrors{0};
    std::atomic<int> okCount{0}; // successful calls whose buckets were inspected
    std::string lastViolation;
    std::mutex violationMx; // serializes the single diagnostic string

    const DDBAddOptions opts{false, false, 2}; // recursive, stopOnError, maxConflictRetries

    // One-shot C++17 barrier (std::countdown_latch is C++20): the last writer to arrive opens
    // the gate; the others spin until it does, so all writers fire the add in the same wave.
    std::atomic<int> arrived{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < T; ++t) {
        threads.emplace_back([&] {
            if (arrived.fetch_add(1, std::memory_order_acq_rel) + 1 == T)
                go.store(true, std::memory_order_release);
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();

            char *out = nullptr;
            const auto err = DDBAddWithOptions(root.string().c_str(), pathPtrs.data(), N, &opts,
                                               &out);
            if (err != DDBERR_NONE) {
                // No completed result, so nothing to inspect: not a contract violation.
                if (err == DDBERR_BUSY)
                    ++busy;
                else
                    ++otherErrors;
                DDBFree(out); // no-op when out is nullptr
                return;
            }

            // An uncaught exception escaping the worker would std::terminate the whole run.
            json j;
            try {
                j = json::parse(out);
            } catch (const std::exception &e) {
                DDBFree(out);
                ++otherErrors; // malformed output is an API-level defect
                return;
            }
            DDBFree(out);
            ++okCount;

            const int entries = static_cast<int>(j["entries"].size());
            const int unchanged = static_cast<int>(j["unchanged"].size());
            const int errors = static_cast<int>(j["errors"].size());

            if (entries + unchanged + errors != N) {
                ++violations;
                std::lock_guard<std::mutex> lk(violationMx);
                lastViolation = "entries=" + std::to_string(entries) +
                                " unchanged=" + std::to_string(unchanged) +
                                " errors=" + std::to_string(errors) +
                                " input=" + std::to_string(N);
            } else {
                // A double-count could mask a vanish that still sums to N, so separately check
                // that every path lands in exactly one bucket.
                std::set<std::string> seen;
                for (const auto &e : j["entries"])
                    if (!seen.insert(e["path"].get<std::string>()).second) ++duplicates;
                for (const auto &e : j["unchanged"])
                    if (!seen.insert(e["path"].get<std::string>()).second) ++duplicates;
                for (const auto &e : j["errors"])
                    if (!seen.insert(e["path"].get<std::string>()).second) ++duplicates;
            }
        });
    }
    for (auto &th : threads) th.join();

    if (violations.load() != 0 || otherErrors.load() != 0)
        fprintf(stderr, "[completeness] busy=%d otherErrors=%d violations=%d last=\"%s\"\n",
                busy.load(), otherErrors.load(), violations.load(), lastViolation.c_str());

    EXPECT_EQ(violations.load(), 0);
    EXPECT_EQ(duplicates.load(), 0);
    // A run where every call failed would pass the contract checks above with zero coverage,
    // so fail loudly on non-busy errors and require at least one inspected result.
    EXPECT_EQ(otherErrors.load(), 0);
    EXPECT_GE(okCount.load(), 1);
}

} // namespace
