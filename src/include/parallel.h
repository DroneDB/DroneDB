/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PARALLEL_H
#define PARALLEL_H

#include <cstddef>
#include <functional>
#include <string>

#include "ddb_export.h"

namespace ddb::parallel
{
    // Cores reported by the system, at least 1.
    DDB_DLL size_t hardwareThreads();

    // Thread count wanted by the config option `name` (environment variable or
    // CPLSetConfigOption): N > 0 is N, 0 is every core, unset or invalid is
    // min(defaultMax, cores). The CPU pool decides how many of them run.
    DDB_DLL int threadsOption(const char *name, int defaultMax);

    // ---- The process-wide CPU token pool (DQ-15, XI-06) ----------------------------------
    //
    // One pool of ProcessConfig::cpuPoolSize() tokens (DDBConfigure "cpuPoolSize"; default
    // cores - 1) bounds the CPU threads libddb runs at once beside the calling threads: the
    // workers of forEachIndex, GDAL's NUM_THREADS, the threads of PDAL readers and the cores of
    // the external tools. A calling thread never needs a token: it always does its own work.
    // Each thread beyond it holds one token while it runs.
    //
    // Tokens are only ever tried, never waited for: a parallel section that gets fewer tokens
    // than it wants runs on fewer threads (with none, on the calling thread only), so nested
    // sections and concurrent calls can never deadlock on the pool, whatever its size (0 too).

    // Tokens held by one parallel section, given back when it ends (RAII). Not thread-safe:
    // one section, one owner.
    class DDB_DLL CpuTokens
    {
    public:
        CpuTokens() noexcept = default;
        ~CpuTokens();
        CpuTokens(CpuTokens &&other) noexcept;
        CpuTokens &operator=(CpuTokens &&other) noexcept;
        CpuTokens(const CpuTokens &) = delete;
        CpuTokens &operator=(const CpuTokens &) = delete;

        // Tokens held
        size_t count() const noexcept { return count_; }
        // Threads the section may run: the calling thread and one per token
        size_t threads() const noexcept { return count_ + 1; }

        // Takes up to n more tokens from the pool without waiting; returns how many it took
        size_t tryAcquire(size_t n) noexcept;
        // Gives back n of the tokens held (all of them when n is larger)
        void release(size_t n) noexcept;

    private:
        size_t count_ = 0;
    };

    // Tokens for a section that wants `threads` threads in all (the calling thread included,
    // so it asks for threads - 1 tokens): 0 to threads - 1 of them, without waiting
    DDB_DLL CpuTokens tryAcquireThreads(size_t threads) noexcept;

    // "NUM_THREADS=<tokens.threads()>": GDAL's thread option (creation option -co, warp option
    // -wo) for the threads the tokens allow. Keep the tokens alive while GDAL uses the threads
    // (until the dataset that was created with the option is closed).
    DDB_DLL std::string gdalNumThreads(const CpuTokens &tokens);

    // Tokens held in the whole process (diagnostics and tests)
    DDB_DLL size_t cpuTokensInUse() noexcept;

    // ---- Parallel loops ---------------------------------------------------------------------

    // Calls fn(i) once for every i in [0, count), on up to `maxThreads` threads
    // (the calling thread is one of them; 0 or 1 runs everything inline).
    // The threads beyond the calling one take CPU tokens: as many as the pool
    // has free when the call starts, then the calling thread takes another one
    // after each of its calls while fewer threads run than wanted, so a section
    // grows when another one ends.
    // Indexes are handed out in increasing order. When a call throws, no new
    // index starts, the calls already running finish, and the exception of the
    // lowest failed index is rethrown: the same one a serial loop would throw.
    // GDAL errors raised by fn go to GDAL's default handler on every thread, not
    // to the process-wide handler (which PDAL replaces while its stages run).
    DDB_DLL void forEachIndex(size_t count, size_t maxThreads, const std::function<void(size_t)> &fn);

    // forEachIndex for a section bound by the disk rather than by the CPU (reading files to
    // hash them, deleting folders): up to ProcessConfig::ioThreads() threads (DDBConfigure
    // "ioThreads", default 4) per call, without CPU tokens.
    DDB_DLL void forEachIndexIo(size_t count, const std::function<void(size_t)> &fn);

} // namespace ddb::parallel

#endif // PARALLEL_H
