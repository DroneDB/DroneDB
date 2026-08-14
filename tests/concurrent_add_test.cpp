/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Baseline / regression harness for the "database is locked" incident under concurrent DDBAdd
// calls to the same dataset (see DroneDB-Roadmap/WORKING/ImproveParallelWrites).
//
// Phases 1-3 (transaction RAII, plan/compute/commit split, retry policy) are implemented, so this
// is now a real correctness gate (see 06-testing-and-validation.md, Phase 2 exit criteria):
// concurrent DDBAdd calls to the same dataset must not surface SQLITE_BUSY/"database is locked".

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#include "ddb.h"
#include "gtest/gtest.h"
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

// baseline harness described in 06-testing-and-validation.md §2.2 / §1.
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

} // namespace
