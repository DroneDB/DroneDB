/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "c_api_util.h"

#include <atomic>
#include <mutex>
#include <new>

#include "gdal_inc.h"
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
            return "libddb is shut down";
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

DDBErr reportCurrentException(CodeSet allowed) noexcept {
    try {
        const Failure f = classifyCurrentException();
        setLastError(f.message, f.reason);
        return restrictCode(f.code, allowed);
    } catch (...) {
        // The failure could not even be copied
        setLastError("Out of memory");
        return DDBERR_EXCEPTION;
    }
}

DDBErr initializeProcess(ProcessInit init, bool verbose) noexcept {
    clearLastError();
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
