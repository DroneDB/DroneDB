/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_C_API_UTIL_H
#define DDB_C_API_UTIL_H

// The C API adapter (DQ-10, XI-02, XI-03): how ddb.cpp turns C arguments into C++ values and a
// C++ failure into a DDBErr code and the last error. Private to the C API (ddb_capi) and its unit
// tests: nothing here is a DDB* export.
//
// An export is one guarded call:
//
//   DDBErr DDBThing(const char* ddbPath, char** output) {
//       return guardedOutput(output, [&] {
//           requireDdbPath(ddbPath);
//           requireOutput(output);
//           return thing(...);  // std::string or json
//       });
//   }
//
// The mapper clears the last error on entry, catches every exception, sets a message for every
// code but DDBERR_NONE, and writes the outputs only after the body returned.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ddb.h"
#include "exceptions.h"
#include "fs.h"
#include "json.h"

namespace ddb::capi {

// ------------------------------------------------------------------ last error (XI-02)

// The last error of the calling thread: the message and the reason of its last failed call.
// Every guarded call clears it on entry. The exempt accessors (DDBGetLastError,
// DDBGetLastErrorReason, DDBGetVersion, DDBGetAbiVersion, DDBGetCapabilities, DDBFree,
// DDBVSIFree, DDBFreeCancelToken) never touch it.
void clearLastError() noexcept;
// Stores message and reason, whole (no length limit). When they cannot be stored (out of memory)
// the message reads "Out of memory" and the reason is "".
void setLastError(std::string_view message, std::string_view reason = {}) noexcept;
// Never NULL, "" when there is none. Valid until the next call on this thread that sets or
// clears the last error.
const char* lastErrorMessage() noexcept;
const char* lastErrorReason() noexcept;

// The reasons of the retryable failures (DDBGetLastErrorReason), by RetryableBuildReason. The
// names are part of the ABI: one is only ever added. DDBGetCapabilities() lists them.
inline constexpr std::array<std::pair<RetryableBuildReason, std::string_view>, 3> kRetryableReasons = {{
    {RetryableBuildReason::IdleTimeout, "idleTimeout"},
    {RetryableBuildReason::LowDiskSpace, "lowDiskSpace"},
    {RetryableBuildReason::NoSpaceOnDevice, "noSpaceOnDevice"},
}};

// ------------------------------------------------------------------ codes

// A set of DDBErr codes, one bit per code
using CodeSet = std::uint32_t;

constexpr CodeSet codeBit(DDBErr code) noexcept {
    return CodeSet{1} << static_cast<unsigned>(code);
}

template <typename... Codes>
constexpr CodeSet codeSet(Codes... codes) noexcept {
    return (CodeSet{0} | ... | codeBit(codes));
}

// The codes an export returns, besides DDBERR_NONE and DDBERR_EXCEPTION (RETURN-CODE FREEZE).
// The exports of 1.x keep the codes they returned before the mapper: DDBERR_BUSY for database
// contention, plus the code a few of them always returned for one failure (DDBGet NOTFOUND,
// DDBBuild BUILDDEPMISSING/BUILDINPROGRESS, DDBExportRaster2 CANCELED: see ddb.cpp). Any other
// failure stays DDBERR_EXCEPTION, with its own message and reason. DDBERR_INVALIDARGS and
// DDBERR_RETRYABLE are returned by the exports added in 2.0 only; the older exports switch
// in a later version, together with Registry (RQ-08).
inline constexpr CodeSet kFrozenCodes = codeSet(DDBERR_BUSY);
// The exports added in 2.0: every code a failure maps to
inline constexpr CodeSet kNewExportCodes = codeSet(DDBERR_BUSY,
                                               DDBERR_NOTFOUND,
                                               DDBERR_INVALIDARGS,
                                               DDBERR_RETRYABLE,
                                               DDBERR_BUILDDEPMISSING,
                                               DDBERR_BUILDINPROGRESS,
                                               DDBERR_CANCELED);

// A failure with the most specific code it maps to
struct Failure {
    DDBErr code = DDBERR_EXCEPTION;
    std::string message;  // never empty
    std::string reason;   // "" when the failure has none
};

// Maps the exception being handled; call it inside a catch block only:
//   DBBusyException            BUSY
//   EntryNotFoundException     NOTFOUND
//   BuildDepMissingException   BUILDDEPMISSING
//   BuildInProgressException   BUILDINPROGRESS
//   ShutdownException          SHUTDOWN (a call stopped by DDBShutdown)
//   CanceledException          CANCELED
//   InvalidArgsException       INVALIDARGS
//   RetryableBuildException    RETRYABLE, reason from kRetryableReasons
//   std::bad_alloc             EXCEPTION, "Out of memory"
//   anything else              EXCEPTION, what() ("Unknown error occurred" when not an exception)
// A failure without a message gets the default message of its code.
Failure classifyCurrentException();

// The code an export returns for a failure: the failure's code when the export may return it,
// DDBERR_EXCEPTION otherwise
constexpr DDBErr restrictCode(DDBErr code, CodeSet allowed) noexcept {
    return (codeBit(code) & allowed) != 0 ? code : DDBERR_EXCEPTION;
}

// Sets the last error from the exception being handled (call it inside a catch block only) and
// returns its code, restricted to allowed. The message and the reason are those of the failure
// whatever the code: a retryable failure of an export of 1.x returns DDBERR_EXCEPTION with its
// reason.
DDBErr reportCurrentException(CodeSet allowed) noexcept;

// Sets the last error of a call refused because DDBShutdown started; returns DDBERR_SHUTDOWN
DDBErr reportShutdown() noexcept;

// The codes a call may return besides its own (allowed): DDBERR_SHUTDOWN always (DDBShutdown),
// DDBERR_CANCELED when the caller set a cancel token on the thread (DDBSetThreadCancelToken).
// A failure of a call whose work was asked to stop (a canceled token, the shutdown) is
// reported as that, when it is not a more specific failure than DDBERR_EXCEPTION.
CodeSet withRuntimeCodes(CodeSet allowed) noexcept;

// ------------------------------------------------------------------ calls in flight and shutdown (XI-07)

// What a guarded call is to the shutdown (DDBShutdown)
enum class CallKind {
    // An export that works on data: refused with DDBERR_SHUTDOWN once DDBShutdown started,
    // counted in flight while it runs (DDBShutdown waits for it)
    Operation,
    // An export that only changes a setting (DDBSetLogCallback, DDBSetThreadOperation, ...):
    // refused and counted like an operation, but it is not "the call" that a thread's
    // per-call state is for, so its exit does not reset that state
    Setting,
    // Works in any state and is not counted (DDBCancel: a host cancels while it shuts down)
    Unrestricted,
};

// One guarded call on this thread (RAII). The outermost call of a thread is admitted unless
// DDBShutdown started, and counted while it runs; a call nested in it (an export called from
// a progress callback) is admitted with it.
class CallScope {
   public:
    explicit CallScope(CallKind kind) noexcept;
    ~CallScope();
    CallScope(const CallScope&) = delete;
    CallScope& operator=(const CallScope&) = delete;

    // False when the call must return DDBERR_SHUTDOWN without running
    bool admitted() const noexcept { return admitted_; }

    // The guarded calls running on this thread (0 outside any)
    static int depth() noexcept;

   private:
    CallKind kind_;
    bool admitted_ = false;
    bool counted_ = false;
};

// Guarded calls admitted and still running, on every thread (diagnostics and tests)
int callsInFlight() noexcept;

// What DDBShutdown does once no call is in flight, before it returns DDBERR_NONE: the
// destructive part (closing the pooled connections and the raster cache). It may throw.
using Teardown = void (*)();

// DDBShutdown(timeoutMs), see ddb.h: refuses new calls and stops the work in flight
// (cancellation.h), waits up to timeoutMs (< 0: without a limit) for the calls in flight, then
// runs teardown once, unregisters the log callback and flushes the log. On timeout it
// unregisters the callback, flushes the log and returns DDBERR_BUSY without the teardown,
// which a later DDBShutdown finishes. Called from inside a guarded call (a progress callback)
// or the log callback: DDBERR_INVALIDARGS. Never counted in flight itself.
DDBErr shutdown(int timeoutMs, Teardown teardown) noexcept;

// True once DDBShutdown started
bool shutdownStarted() noexcept;

// Unit tests only: back to the running state after a test called DDBShutdown (the teardown
// leaves nothing that a later call cannot recreate). Never called by the library.
void resetShutdownForTests() noexcept;

// ------------------------------------------------------------------ process initialization

// DDBRegisterProcess: runs init(verbose). A failure is sticky: this call and every later guarded
// call (DDBRegisterProcess too) return DDBERR_EXCEPTION with the initialization message. After
// DDBShutdown it returns DDBERR_SHUTDOWN, like every guarded call.
using ProcessInit = void (*)(bool verbose);
DDBErr initializeProcess(ProcessInit init, bool verbose) noexcept;
// The message of a failed initialization, or nullptr
const std::string* initFailure() noexcept;

// ------------------------------------------------------------------ the mapper

// Runs body and returns DDBERR_NONE, or the code of the exception it threw (restricted to
// Allowed and withRuntimeCodes). The last error is cleared on entry and set for every other
// code. The outermost Operation of a thread resets the thread's cancel token and operation id
// when it returns.
template <CodeSet Allowed = kFrozenCodes, CallKind Kind = CallKind::Operation, typename Body>
DDBErr guarded(Body&& body) noexcept {
    static_assert(std::is_void_v<std::invoke_result_t<Body&&>>,
                  "a guarded body reports a failure by throwing; use guardedOutput for a result");
    clearLastError();
    const CallScope scope(Kind);
    if (!scope.admitted())
        return reportShutdown();
    if (const std::string* failure = initFailure()) {
        setLastError(*failure);
        return DDBERR_EXCEPTION;
    }
    try {
        std::forward<Body>(body)();
        return DDBERR_NONE;
    } catch (...) {
        return reportCurrentException(withRuntimeCodes(Allowed));
    }
}

// Copies text into *output (not written when it is NULL), in memory the caller frees with DDBFree
void writeString(char** output, const std::string& text);

// guarded() for an export with one output: body returns the value, which is written to *output
// only when body returned (a NULL output is skipped: the body checks a required one). For a
// string output (char**) the value is a std::string or a json, copied into memory the caller
// frees with DDBFree.
template <CodeSet Allowed = kFrozenCodes, typename T, typename Body>
DDBErr guardedOutput(T* output, Body&& body) noexcept {
    return guarded<Allowed>([&] {
        auto value = std::forward<Body>(body)();
        if (output == nullptr)
            return;
        if constexpr (std::is_same_v<T, char*>) {
            if constexpr (std::is_same_v<decltype(value), json>)
                writeString(output, value.dump());
            else
                writeString(output, value);
        } else {
            *output = static_cast<T>(value);
        }
    });
}

// A buffer the library allocates with VSIMalloc and the caller frees with DDBVSIFree. It is
// freed when the export fails, so a failure never leaks it nor hands it out.
class BufferOutput {
   public:
    BufferOutput() = default;
    BufferOutput(const BufferOutput&) = delete;
    BufferOutput& operator=(const BufferOutput&) = delete;
    ~BufferOutput();

    // Where the library writes the buffer and its size
    uint8_t** buffer() noexcept { return &buffer_; }
    int* size() noexcept { return &size_; }

    // Hands the buffer to the caller
    void release(uint8_t** outBuffer, int* outBufferSize) noexcept;

   private:
    uint8_t* buffer_ = nullptr;
    int size_ = 0;
};

// guarded() for an export with a buffer output: body(buffer, size) writes into a BufferOutput,
// which is handed to *outBuffer and *outBufferSize only when body returned. The body checks the
// caller's pointers (requireOutput).
template <CodeSet Allowed = kFrozenCodes, typename Body>
DDBErr guardedBuffer(uint8_t** outBuffer, int* outBufferSize, Body&& body) noexcept {
    return guarded<Allowed>([&] {
        BufferOutput out;
        std::forward<Body>(body)(out.buffer(), out.size());
        out.release(outBuffer, outBufferSize);
    });
}

// ------------------------------------------------------------------ arguments

// The checks of a C argument, with no exception: the require* functions below throw when one
// fails. (Before DQ-05 they were utils::isNullOrEmptyOrWhitespace, isValid*Param and
// hasNullStringInArray.)

// True when text is NULL, empty or whitespace only
bool isNullOrEmptyOrWhitespace(const char* text) noexcept;
// True when any of the count strings is NULL, empty or whitespace only; also for a NULL array
// and for count 0 (nothing was provided)
bool isNullOrEmptyOrWhitespace(const char** items, int count) noexcept;
// True when the array is NULL, count is negative, or any of the count strings is NULL
bool hasNullStringInArray(const char** items, int count) noexcept;
// False for a negative count, or a NULL array with a positive count
bool isValidArrayParam(const char** array, int count) noexcept;
// A string that may be empty but not NULL
bool isValidStringParam(const char* text) noexcept;
// A string that is not NULL and not empty (whitespace is accepted)
bool isValidNonEmptyStringParam(const char* text) noexcept;

// Throws InvalidArgsException(message) when text is NULL, empty or whitespace only
void requireText(const char* text, const char* message);
// The dataset folder (ddbPath): requireText with "No directory provided"
void requireDdbPath(const char* ddbPath);

// Throws InvalidArgsException(message) when output is NULL
template <typename T>
void requireOutput(T* output, const char* message = "Output pointer is null") {
    if (output == nullptr)
        throw InvalidArgsException(message);
}

// An options struct that starts with uint32_t structSize (XI-03), copied: the caller's
// structSize must cover every field of the first version of the struct (minSize), and the
// bytes a newer caller adds beyond sizeof(T) must be zero, since this library cannot honour an
// option it does not know. Throws InvalidArgsException otherwise, or when options is NULL.
template <typename T>
T requireOptions(const T* options, size_t minSize, const char* name) {
    if (options == nullptr)
        throw InvalidArgsException(std::string("No ") + name + " provided");
    const size_t size = options->structSize;
    if (size < minSize)
        throw InvalidArgsException(std::string(name) + "->structSize is " + std::to_string(size) +
                                   ", expected at least " + std::to_string(minSize));
    const auto* bytes = reinterpret_cast<const unsigned char*>(options);
    for (size_t i = sizeof(T); i < size; i++)
        if (bytes[i] != 0)
            throw InvalidArgsException(std::string(name) + " sets a field this libddb does not know (structSize " +
                                       std::to_string(size) + ", known " + std::to_string(sizeof(T)) + ")");
    T copy{};
    std::memcpy(&copy, options, std::min(size, sizeof(T)));
    return copy;
}

// An optional string: "" for NULL
inline std::string optStr(const char* text) {
    return text != nullptr ? std::string(text) : std::string();
}

// A list of paths that must hold at least one path, none NULL, empty or whitespace only
std::vector<std::string> requirePathList(const char** paths, int numPaths);
// A list of strings, possibly empty, without NULL elements (the array itself is not NULL)
std::vector<std::string> toStringList(const char** items, int count);
// JSON text; a NULL or malformed text throws InvalidArgsException
json parseJsonArg(const char* text);

// A path argument (UTF-8) as a path, whatever the locale (io::fromUtf8, XI-03); the caller has
// checked that it is not NULL
fs::path pathArg(const char* utf8);

}  // namespace ddb::capi

#endif  // DDB_C_API_UTIL_H
