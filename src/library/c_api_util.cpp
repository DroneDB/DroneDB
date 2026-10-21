/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "c_api_util.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>

#include "cancellation.h"
#include "gdal_inc.h"
#include "logger.h"
#include "mio.h"
#include "utils.h"

namespace ddb::capi {

namespace {

struct LastError {
    std::string message;
    std::string reason;
    bool outOfMemory = false;  // message and reason could not be stored
};

LastError& lastError() noexcept {
    thread_local LastError error;
    return error;
}

// Every code but NONE has a message: the default one of its code when the failure has none
const char* defaultMessage(DDBErr code) noexcept {
    switch (code) {
        case DDBERR_NONE:
            return "";
        case DDBERR_BUILDDEPMISSING:
            return "Build skipped: a dependency is missing";
        case DDBERR_BUILDINPROGRESS:
            return "Build skipped: another process holds the build lock";
        case DDBERR_CANCELED:
            return "Canceled by the progress callback";
        case DDBERR_BUSY:
            return "The database is busy";
        case DDBERR_NOTFOUND:
            return "The entry does not exist";
        case DDBERR_INVALIDARGS:
            return "Invalid arguments";
        case DDBERR_RETRYABLE:
            return "Transient failure: retry later";
        case DDBERR_SHUTDOWN:
            return "libddb is shut down (DDBShutdown): the call did not run";
        case DDBERR_EXCEPTION:
            break;
    }
    return "Unknown error occurred";
}

std::string_view retryableReason(RetryableBuildReason reason) noexcept {
    for (const auto& [value, name] : kRetryableReasons)
        if (value == reason)
            return name;
    return {};
}

Failure makeFailure(DDBErr code, const char* message, std::string_view reason = {}) {
    Failure f;
    f.code = code;
    f.message = message != nullptr && *message != '\0' ? message : defaultMessage(code);
    f.reason = reason;
    return f;
}

// The initialization failure: written once, under initMutex, before initFailed is set
std::mutex initMutex;
std::string initFailureMessage;
std::atomic<bool> initFailed{false};

// ---- calls in flight and shutdown
//
// One atomic word: the guarded calls in flight in the low bits, kShutdownBit once DDBShutdown
// started. A call is admitted by a compare-exchange that fails once the bit is set, so
// admission and the shutdown are totally ordered: once DDBShutdown has set the bit, the count
// it reads only goes down.
constexpr std::uint64_t kShutdownBit = std::uint64_t{1} << 63;
std::atomic<std::uint64_t> callWord{0};

// DDBShutdown waits on drained for the count to reach 0; a call leaving after the bit was set
// notifies it under drainMutex, so the wakeup cannot fall between the waiter's check and wait
std::mutex drainMutex;
std::condition_variable drained;

// Serializes DDBShutdown calls; tornDown (under it) once the teardown ran
std::timed_mutex shutdownMutex;
bool tornDown = false;

thread_local int callDepth = 0;

std::uint64_t callsOf(std::uint64_t word) noexcept {
    return word & ~kShutdownBit;
}

// Takes a call out of the count; wakes DDBShutdown when it is waiting for the count
void leaveCall() noexcept {
    const std::uint64_t before = callWord.fetch_sub(1);
    if ((before & kShutdownBit) != 0) {
        // std::mutex::lock throws only on a system error (EDEADLK, EINVAL), never here
        const std::lock_guard lock(drainMutex);
        drained.notify_all();
    }
}

// Waits until no call is in flight: up to timeoutMs, without a limit when it is negative.
// True when none is left.
bool waitForCalls(int timeoutMs) {
    const auto none = [] { return callsOf(callWord.load()) == 0; };
    std::unique_lock lock(drainMutex);
    if (timeoutMs < 0) {
        drained.wait(lock, none);
        return true;
    }
    return drained.wait_for(lock, std::chrono::milliseconds(timeoutMs), none);
}

std::string callsLeftMessage(int timeoutMs) {
    const auto left = callsOf(callWord.load());
    return "libddb is shutting down: " + std::to_string(left) + (left == 1 ? " call" : " calls") +
           " still running after " + std::to_string(timeoutMs) +
           " ms; new calls are refused, and a later DDBShutdown finishes the shutdown";
}

}  // namespace

void clearLastError() noexcept {
    auto& error = lastError();
    error.message.clear();  // keeps the capacity: never allocates
    error.reason.clear();
    error.outOfMemory = false;
}

void setLastError(std::string_view message, std::string_view reason) noexcept {
    auto& error = lastError();
    try {
        error.message.assign(message);
        error.reason.assign(reason);
        error.outOfMemory = false;
    } catch (...) {
        error.message.clear();
        error.reason.clear();
        error.outOfMemory = true;
    }
}

const char* lastErrorMessage() noexcept {
    const auto& error = lastError();
    return error.outOfMemory ? "Out of memory" : error.message.c_str();
}

const char* lastErrorReason() noexcept {
    const auto& error = lastError();
    return error.outOfMemory ? "" : error.reason.c_str();
}

Failure classifyCurrentException() {
    try {
        throw;
    } catch (const DBBusyException& e) {
        return makeFailure(DDBERR_BUSY, e.what());
    } catch (const EntryNotFoundException& e) {
        return makeFailure(DDBERR_NOTFOUND, e.what());
    } catch (const BuildDepMissingException& e) {
        return makeFailure(DDBERR_BUILDDEPMISSING, e.what());
    } catch (const BuildInProgressException& e) {
        return makeFailure(DDBERR_BUILDINPROGRESS, e.what());
    } catch (const ShutdownException& e) {
        return makeFailure(DDBERR_SHUTDOWN, e.what());
    } catch (const CanceledException& e) {
        return makeFailure(DDBERR_CANCELED, e.what());
    } catch (const InvalidArgsException& e) {
        return makeFailure(DDBERR_INVALIDARGS, e.what());
    } catch (const RetryableBuildException& e) {
        return makeFailure(DDBERR_RETRYABLE, e.what(), retryableReason(e.reason()));
    } catch (const std::bad_alloc&) {
        return makeFailure(DDBERR_EXCEPTION, "Out of memory");
    } catch (const std::exception& e) {
        return makeFailure(DDBERR_EXCEPTION, e.what());
    } catch (...) {
        return makeFailure(DDBERR_EXCEPTION, nullptr);
    }
}

// A generic failure (DDBERR_EXCEPTION) of work that was asked to stop is the stop: GDAL fails a
// canceled call ("User terminated"), SQLite an interrupted statement
Failure attributeToCancellation(Failure f) {
    if (f.code != DDBERR_EXCEPTION)
        return f;
    if (cancel::shutdownRequested())
        return Failure{DDBERR_SHUTDOWN, std::string(cancel::kShuttingDown) + ": " + f.message, ""};
    if (const auto token = cancel::threadToken(); token && token.get()->canceled())
        return Failure{DDBERR_CANCELED, std::string(token.get()->message()) + ": " + f.message, ""};
    return f;
}

CodeSet withRuntimeCodes(CodeSet allowed) noexcept {
    return allowed | codeBit(DDBERR_SHUTDOWN) | (cancel::hasThreadToken() ? codeBit(DDBERR_CANCELED) : 0);
}

DDBErr reportCurrentException(CodeSet allowed) noexcept {
    try {
        const Failure f = attributeToCancellation(classifyCurrentException());
        setLastError(f.message, f.reason);
        return restrictCode(f.code, allowed);
    } catch (...) {
        // The failure could not even be copied
        setLastError("Out of memory");
        return DDBERR_EXCEPTION;
    }
}

DDBErr reportShutdown() noexcept {
    setLastError(defaultMessage(DDBERR_SHUTDOWN));
    return DDBERR_SHUTDOWN;
}

CallScope::CallScope(CallKind kind) noexcept : kind_(kind) {
    ++callDepth;
    if (kind_ == CallKind::Unrestricted || callDepth > 1) {
        // Nested in an admitted call, which is counted already
        admitted_ = true;
        return;
    }
    // A refused call never touches the count, so refused calls cannot keep DDBShutdown waiting
    std::uint64_t word = callWord.load();
    do {
        if ((word & kShutdownBit) != 0)
            return;
    } while (!callWord.compare_exchange_weak(word, word + 1));
    admitted_ = true;
    counted_ = true;
}

CallScope::~CallScope() {
    if (counted_)
        leaveCall();
    // The token the caller set was for this call (DDBSetThreadCancelToken)
    if (callDepth == 1 && kind_ == CallKind::Operation)
        cancel::setThreadToken(nullptr);
    --callDepth;
}

int CallScope::depth() noexcept {
    return callDepth;
}

int callsInFlight() noexcept {
    return static_cast<int>(callsOf(callWord.load()));
}

bool shutdownStarted() noexcept {
    return (callWord.load() & kShutdownBit) != 0;
}

DDBErr shutdown(int timeoutMs, Teardown teardown) noexcept {
    clearLastError();
    if (CallScope::depth() > 0) {
        // It would wait for the call it runs in
        setLastError("DDBShutdown cannot be called from inside a libddb call (a progress callback)");
        return DDBERR_INVALIDARGS;
    }
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
        // A concurrent DDBShutdown holds it while it waits or tears down: wait for it in the
        // same time budget
        std::unique_lock serialize(shutdownMutex, std::defer_lock);
        if (timeoutMs < 0)
            serialize.lock();
        else if (!serialize.try_lock_until(deadline)) {
            setLastError(callsLeftMessage(timeoutMs));
            return DDBERR_BUSY;
        }
        if (tornDown)
            return DDBERR_NONE;

        callWord.fetch_or(kShutdownBit);
        cancel::requestShutdown();  // the calls in flight stop at their next checkpoint
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (!waitForCalls(timeoutMs < 0 ? -1 : static_cast<int>(std::max<long long>(left.count(), 0)))) {
            // Not destructive: the calls still running keep their connections and datasets
            flush_logger();
            setLastError(callsLeftMessage(timeoutMs));
            return DDBERR_BUSY;
        }

        // Once, even when a step fails: a retry would find half of it done
        tornDown = true;
        DDBErr result = DDBERR_NONE;
        try {
            teardown();
        } catch (...) {
            setLastError("DDBShutdown could not close everything: " + classifyCurrentException().message);
            result = DDBERR_EXCEPTION;
        }
        flush_logger();
        return result;
    } catch (...) {
        // The mutex (a system error) or the message: the shutdown flag may be set
        setLastError("DDBShutdown failed: out of memory or system error");
        return DDBERR_EXCEPTION;
    }
}

void resetShutdownForTests() noexcept {
    const std::lock_guard serialize(shutdownMutex);
    callWord.fetch_and(~kShutdownBit);
    cancel::resetShutdownForTests();
    tornDown = false;
}

DDBErr initializeProcess(ProcessInit init, bool verbose) noexcept {
    clearLastError();
    const CallScope scope(CallKind::Operation);
    if (!scope.admitted())
        return reportShutdown();
    try {
        const std::lock_guard lock(initMutex);
        if (const std::string* failure = initFailure()) {
            setLastError(*failure);
            return DDBERR_EXCEPTION;
        }
        try {
            init(verbose);
            return DDBERR_NONE;
        } catch (...) {
            initFailureMessage = "libddb initialization failed: " + classifyCurrentException().message;
            initFailed.store(true, std::memory_order_release);
            setLastError(initFailureMessage);
            return DDBERR_EXCEPTION;
        }
    } catch (...) {
        // The mutex or the message: the initialization did not run
        setLastError("libddb initialization failed: out of memory or system error");
        return DDBERR_EXCEPTION;
    }
}

const std::string* initFailure() noexcept {
    return initFailed.load(std::memory_order_acquire) ? &initFailureMessage : nullptr;
}

void writeString(char** output, const std::string& text) {
    utils::copyToPtr(text, output);
}

BufferOutput::~BufferOutput() {
    if (buffer_ != nullptr)
        VSIFree(buffer_);
}

void BufferOutput::release(uint8_t** outBuffer, int* outBufferSize) noexcept {
    if (outBuffer != nullptr) {
        *outBuffer = buffer_;
        buffer_ = nullptr;
    }
    if (outBufferSize != nullptr)
        *outBufferSize = size_;
}

void requireText(const char* text, const char* message) {
    if (utils::isNullOrEmptyOrWhitespace(text))
        throw InvalidArgsException(message);
}

void requireDdbPath(const char* ddbPath) {
    requireText(ddbPath, "No directory provided");
}

std::vector<std::string> requirePathList(const char** paths, int numPaths) {
    if (!utils::isValidArrayParam(paths, numPaths))
        throw InvalidArgsException("Invalid paths array parameter");
    // NULL elements are blank too
    if (utils::isNullOrEmptyOrWhitespace(paths, numPaths))
        throw InvalidArgsException("No paths provided");
    return std::vector<std::string>(paths, paths + numPaths);
}

std::vector<std::string> toStringList(const char** items, int count) {
    if (!utils::isValidArrayParam(items, count))
        throw InvalidArgsException("Invalid paths array parameter");
    // A NULL array is rejected too, even when empty
    if (utils::hasNullStringInArray(items, count))
        throw InvalidArgsException("Path array contains null elements");
    return std::vector<std::string>(items, items + count);
}

json parseJsonArg(const char* text) {
    if (text == nullptr)
        throw InvalidArgsException("No JSON provided");
    try {
        return json::parse(text);
    } catch (const json::parse_error& e) {
        throw InvalidArgsException(e.what());
    }
}

fs::path pathArg(const char* utf8) {
    return io::fromUtf8(utf8);
}

}  // namespace ddb::capi
