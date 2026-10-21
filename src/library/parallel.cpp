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

#include "gdal_inc.h"

namespace ddb::parallel
{
    namespace
    {
        // Sends the GDAL errors raised on this thread to GDAL's default handler (stderr, CPL_LOG)
        // while in scope, whatever process-wide handler is installed: a thread-local handler
        // takes precedence over the process-wide one. While any PDAL stage runs, PDAL installs
        // its own process-wide GDAL handler (pdal::Stage::startLogging), which writes into that
        // stage's Log; that Log is not synchronized with the stage's thread, so a GDAL error
        // raised by another thread at the same time races with it (TSan, DQ-01). Parallel
        // sections parse rasters while other threads run PDAL readers.
        class ThreadGdalErrorHandler
        {
        public:
            ThreadGdalErrorHandler() { CPLPushErrorHandler(CPLDefaultErrorHandler); }
            ~ThreadGdalErrorHandler() { CPLPopErrorHandler(); }
            ThreadGdalErrorHandler(const ThreadGdalErrorHandler &) = delete;
            ThreadGdalErrorHandler &operator=(const ThreadGdalErrorHandler &) = delete;
        };
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
                return static_cast<int>(parsed);
        }
        return static_cast<int>(std::min(hardwareThreads(), static_cast<size_t>(std::max(1, defaultMax))));
    }

    void forEachIndex(size_t count, size_t maxThreads, const std::function<void(size_t)> &fn)
    {
        const ThreadGdalErrorHandler callerGdalErrors; // the calling thread runs calls too
        const size_t threads = std::min(count, std::max<size_t>(1, maxThreads));
        if (threads <= 1)
        {
            for (size_t i = 0; i < count; i++)
                fn(i);
            return;
        }

        std::atomic<size_t> next{0};
        // Lowest failed index; indexes below it still run, so the error kept is the serial one
        std::atomic<size_t> firstFailed{count};
        std::mutex errorMutex;
        std::exception_ptr error;

        const auto work = [&]()
        {
            for (size_t i = next++; i < firstFailed; i = next++)
            {
                try
                {
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
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(threads - 1);
        try
        {
            for (size_t t = 1; t < threads; t++)
                workers.emplace_back([&work]()
                                     {
                                         const ThreadGdalErrorHandler workerGdalErrors;
                                         work();
                                     });
        }
        catch (...)
        {
            // Could not start a thread: the ones already running and this one do the work
        }

        work();
        for (auto &w : workers)
            w.join();

        if (error)
            std::rethrow_exception(error);
    }

} // namespace ddb::parallel
