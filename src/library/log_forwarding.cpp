/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "log_forwarding.h"

#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "exceptions.h"

namespace ddb::logfwd
{
    namespace
    {
        // One registration, immutable once published
        struct Sink
        {
            Callback callback = nullptr;
            int minLevel = kTrace;
            void *userData = nullptr;
            // Threads inside callback; retired when the registration is replaced
            std::atomic<int> active{0};
            std::atomic<bool> retired{false};
        };

        std::atomic<Sink *> current{nullptr};

        // Serializes the setters. Every Sink ever published stays allocated (a reader may still
        // hold its pointer after it was replaced): a few bytes per registration, which a host
        // makes once or twice.
        std::mutex setterMutex;
        std::vector<std::unique_ptr<Sink>> &allSinks()
        {
            static auto *sinks = new std::vector<std::unique_ptr<Sink>>(); // never destroyed
            return *sinks;
        }
        bool exitHookInstalled = false; // under setterMutex

        thread_local bool insideCallback = false;
        thread_local uint64_t operationId = 0;

        // Takes a thread out of sink; wakes the setter that waits for it to be idle
        void leave(Sink *sink) noexcept
        {
            sink->active.fetch_sub(1);
            if (sink->retired.load())
                sink->active.notify_all();
        }

        // Waits until no thread is inside sink, which is no longer the current one
        void waitIdle(Sink *sink) noexcept
        {
            sink->retired.store(true);
            for (int n = sink->active.load(); n != 0; n = sink->active.load())
                sink->active.wait(n);
        }

        // At process exit: no callback any more, without waiting (the host's threads may be gone)
        void disableAtExit()
        {
            current.store(nullptr);
        }
    }

    void setCallback(Callback callback, int minLevel, void *userData)
    {
        if (insideCallback)
            throw InvalidArgsException("DDBSetLogCallback cannot be called from inside the log callback");
        if (callback != nullptr && (minLevel < kTrace || minLevel > kFatal))
            throw InvalidArgsException("minLevel must be a DDBLogLevel (" + std::to_string(kTrace) + ".." +
                                       std::to_string(kFatal) + "), not " + std::to_string(minLevel));

        const std::lock_guard lock(setterMutex);
        Sink *next = nullptr;
        if (callback != nullptr)
        {
            auto sink = std::make_unique<Sink>();
            sink->callback = callback;
            sink->minLevel = minLevel;
            sink->userData = userData;
            allSinks().push_back(std::move(sink));
            next = allSinks().back().get();
            if (!exitHookInstalled)
            {
                // Runs before libddb's own exit-time handlers registered earlier and before
                // GDAL's library destructor: no callback into a host that is exiting
                std::atexit(&disableAtExit);
                exitHookInstalled = true;
            }
        }
        if (Sink *previous = current.exchange(next))
            waitIdle(previous);
    }

    void unregister()
    {
        setCallback(nullptr, kTrace, nullptr);
    }

    int callbackMinLevel() noexcept
    {
        const Sink *sink = current.load();
        return sink != nullptr ? sink->minLevel : -1;
    }

    bool deliver(int level, const char *source, const char *message) noexcept
    {
        if (insideCallback)
            return true; // a record logged while this thread runs the callback: dropped
        Sink *sink = current.load();
        if (sink == nullptr)
            return false;
        sink->active.fetch_add(1);
        // Replaced meanwhile: the setter may not wait for this thread, so do not call it
        if (current.load() != sink)
        {
            leave(sink);
            return false;
        }
        if (level >= sink->minLevel)
        {
            insideCallback = true;
            try
            {
                sink->callback(level, message != nullptr ? message : "", source, operationId, sink->userData);
            }
            catch (...)
            {
                // A C callback cannot throw; a C++ one that does loses that record only
            }
            insideCallback = false;
        }
        leave(sink);
        return true;
    }

    bool inCallback() noexcept
    {
        return insideCallback;
    }

    void setThreadOperation(uint64_t opId) noexcept
    {
        operationId = opId;
    }

    uint64_t threadOperation() noexcept
    {
        return operationId;
    }

    ScopedOperation::ScopedOperation(uint64_t opId) noexcept : previous_(operationId)
    {
        operationId = opId;
    }

    ScopedOperation::~ScopedOperation()
    {
        operationId = previous_;
    }

    int levelOf(CPLErr errorClass) noexcept
    {
        switch (errorClass)
        {
        case CE_Debug:
            return kDebug;
        case CE_Warning:
            return kWarning;
        case CE_Failure:
            return kError;
        case CE_Fatal:
            return kFatal;
        case CE_None:
            break;
        }
        return kInfo;
    }

    void CPL_STDCALL gdalErrorHandler(CPLErr errorClass, CPLErrorNum errorNum, const char *message)
    {
        if (!deliver(levelOf(errorClass), "gdal", message))
            CPLDefaultErrorHandler(errorClass, errorNum, message);
    }

    void installGdalErrorHandler()
    {
        CPLSetErrorHandler(&gdalErrorHandler);
    }

} // namespace ddb::logfwd
