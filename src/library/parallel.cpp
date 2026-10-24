/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "parallel.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include "cancellation.h"
#include "gdal_inc.h"
#include "log_forwarding.h"
#include "process_config.h"

namespace ddb::parallel
{
    namespace
    {
        // Sends the GDAL errors raised on this thread to libddb's handler while in scope: the
        // host's log callback with the thread's operation id, or GDAL's default handler (stderr,
        // CPL_LOG) when none is registered (log_forwarding.h), whatever process-wide handler is
        // installed: a thread-local handler takes precedence over the process-wide one. While any
        // PDAL stage runs, PDAL installs its own process-wide GDAL handler
        // (pdal::Stage::startLogging), which writes into that stage's Log; that Log is not
        // synchronized with the stage's thread, so a GDAL error raised by another thread at the
        // same time races with it (found by TSan). Parallel sections parse rasters while other
        // threads run PDAL readers.
        class ThreadGdalErrorHandler
        {
        public:
            ThreadGdalErrorHandler() { CPLPushErrorHandler(logfwd::gdalErrorHandler); }
            ~ThreadGdalErrorHandler() { CPLPopErrorHandler(); }
            ThreadGdalErrorHandler(const ThreadGdalErrorHandler &) = delete;
            ThreadGdalErrorHandler &operator=(const ThreadGdalErrorHandler &) = delete;
        };

        // The CPU token pool. Never destroyed: threads may still give tokens back while the
        // process exits.
        struct CpuPool
        {
            std::mutex mutex;
            size_t inUse = 0;
        };

        CpuPool &cpuPool()
        {
            static CpuPool *const pool = new CpuPool();
            return *pool;
        }

        // Runs fn(i) for every i in [0, count) on up to `threads` threads (forEachIndex). With
        // `pooled`, every thread beyond the calling one holds a CPU token.
        void runParallel(size_t count, size_t threads, bool pooled, const std::function<void(size_t)> &fn)
        {
            const ThreadGdalErrorHandler callerGdalErrors; // the calling thread runs calls too
            threads = std::min(count, std::max<size_t>(1, threads));
            if (threads <= 1)
            {
                for (size_t i = 0; i < count; i++)
                {
                    cancel::throwIfCanceled();
                    fn(i);
                }
                return;
            }

            std::atomic<size_t> next{0};
            // Lowest failed index; indexes below it still run, so the error kept is the serial one
            std::atomic<size_t> firstFailed{count};
            std::mutex errorMutex;
            std::exception_ptr error;

            const auto runOne = [&](size_t i)
            {
                try
                {
                    // A stop fails the item it would start: the loop ends like on an error
                    cancel::throwIfCanceled();
                    fn(i);
                }
                catch (...)
                {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (i < firstFailed)
                    {
                        firstFailed = i;
                        error = std::current_exception();
                    }
                }
            };
            const auto work = [&]()
            {
                for (size_t i = next++; i < firstFailed; i = next++)
                    runOne(i);
            };

            const size_t wantedWorkers = threads - 1;
            // The workers work for the calling thread's cancel token and operation id
            const cancel::TokenRef callerToken = cancel::threadToken();
            const uint64_t callerOperation = logfwd::threadOperation();
            CpuTokens tokens; // outlives the workers: given back once they are joined
            std::vector<std::thread> workers;
            workers.reserve(wantedWorkers);

            // Starts up to n more workers, each with a token when pooled. False when a thread
            // could not start: the ones already running and this one do the work.
            const auto grow = [&](size_t n)
            {
                const size_t granted = pooled ? tokens.tryAcquire(n) : n;
                size_t started = 0;
                try
                {
                    for (; started < granted; started++)
                        workers.emplace_back([&work, &callerToken, callerOperation]()
                                             {
                                                 const cancel::ScopedThreadToken token(callerToken);
                                                 const logfwd::ScopedOperation operation(callerOperation);
                                                 const ThreadGdalErrorHandler workerGdalErrors;
                                                 work();
                                             });
                }
                catch (...)
                {
                }
                if (pooled)
                    tokens.release(granted - started);
                return started == granted;
            };

            bool canGrow = grow(wantedWorkers);
            // Between its own calls the calling thread takes the tokens other sections gave back
            for (size_t i = next++; i < firstFailed; i = next++)
            {
                runOne(i);
                if (pooled && canGrow && workers.size() < wantedWorkers && next < firstFailed)
                    canGrow = grow(1);
            }
            for (auto &w : workers)
                w.join();

            if (error)
                std::rethrow_exception(error);
        }
    }

    size_t hardwareThreads()
    {
        return std::max(1u, std::thread::hardware_concurrency());
    }

    int threadsOption(const char *name, int defaultMax)
    {
        if (const char *value = CPLGetConfigOption(name, nullptr))
        {
            char *end = nullptr;
            const long parsed = std::strtol(value, &end, 10);
            if (end != value && *end == '\0' && parsed >= 0 && parsed <= std::numeric_limits<int>::max())
                return parsed == 0 ? static_cast<int>(hardwareThreads()) : static_cast<int>(parsed);
        }
        return static_cast<int>(std::min(hardwareThreads(), static_cast<size_t>(std::max(1, defaultMax))));
    }

    CpuTokens::~CpuTokens()
    {
        release(count_);
    }

    CpuTokens::CpuTokens(CpuTokens &&other) noexcept : count_(other.count_)
    {
        other.count_ = 0;
    }

    CpuTokens &CpuTokens::operator=(CpuTokens &&other) noexcept
    {
        if (this != &other)
        {
            release(count_);
            count_ = other.count_;
            other.count_ = 0;
        }
        return *this;
    }

    size_t CpuTokens::tryAcquire(size_t n) noexcept
    {
        if (n == 0)
            return 0;
        try
        {
            const size_t size = ProcessConfig::get().cpuPoolSize();
            auto &pool = cpuPool();
            std::lock_guard<std::mutex> lock(pool.mutex);
            // After the pool shrank, more tokens may be in use than it holds: none is free
            const size_t free = size > pool.inUse ? size - pool.inUse : 0;
            const size_t granted = std::min(n, free);
            pool.inUse += granted;
            count_ += granted;
            return granted;
        }
        catch (...)
        {
            return 0; // no token: the section runs on fewer threads
        }
    }

    void CpuTokens::release(size_t n) noexcept
    {
        n = std::min(n, count_);
        if (n == 0)
            return;
        auto &pool = cpuPool();
        // std::mutex::lock throws only on a system error (EDEADLK, EINVAL), never here
        std::lock_guard<std::mutex> lock(pool.mutex);
        pool.inUse -= n;
        count_ -= n;
    }

    CpuTokens tryAcquireThreads(size_t threads) noexcept
    {
        CpuTokens tokens;
        if (threads > 1)
            tokens.tryAcquire(threads - 1);
        return tokens;
    }

    std::string gdalNumThreads(const CpuTokens &tokens)
    {
        return "NUM_THREADS=" + std::to_string(tokens.threads());
    }

    size_t cpuTokensInUse() noexcept
    {
        auto &pool = cpuPool();
        std::lock_guard<std::mutex> lock(pool.mutex);
        return pool.inUse;
    }

    void forEachIndex(size_t count, size_t maxThreads, const std::function<void(size_t)> &fn)
    {
        runParallel(count, maxThreads, true, fn);
    }

    void forEachIndexIo(size_t count, const std::function<void(size_t)> &fn)
    {
        runParallel(count, ProcessConfig::get().ioThreads(), false, fn);
    }

} // namespace ddb::parallel
