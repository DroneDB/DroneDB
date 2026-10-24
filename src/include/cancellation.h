/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CANCELLATION_H
#define CANCELLATION_H

#include <atomic>
#include <functional>

#include "gdal_inc.h"

struct sqlite3;

// Cancellation of the work of a call (DQ-15, XI-07). Two things stop work: the cancel token of
// the calling thread (DDBSetThreadCancelToken, canceled by DDBCancel or by a progress callback),
// and the shutdown (DDBShutdown), which stops the work of every thread. The work checks them at
// its checkpoints: throwIfCanceled() between steps, every item of parallel::forEachIndex, the
// GDAL progress callbacks (GdalProgress), the SQLite progress handler of every connection, and
// the supervise loop of process::run (which kills the child).
namespace ddb::cancel
{
    // The message of a token canceled by DDBCancel
    inline constexpr const char *kCanceledByToken = "Canceled by the cancel token (DDBCancel)";
    // The message of the work stopped by the shutdown
    inline constexpr const char *kShuttingDown = "Stopped: libddb is shutting down (DDBShutdown)";

    // A cancellation token, the object behind DDBCancelToken. Reference counted: the host holds
    // one reference (DDBCreateCancelToken .. DDBFreeCancelToken), and every thread that works
    // for it holds another one while it does, so the host may free its token at any time, also
    // while a call that uses it is running. Canceling is final. A child token (CallToken) is
    // canceled with its parent too.
    class Token
    {
    public:
        // A new token, not canceled, with one reference: the caller's
        static Token *create(Token *parent = nullptr);

        void retain() noexcept;
        // Drops a reference; the last one deletes the token
        void release() noexcept;

        // Cancels the token; message (a string literal, it is never copied) says why. The first
        // message stays.
        void cancel(const char *message) noexcept;
        bool canceled() const noexcept { return message() != nullptr; }
        // Why the token (or its parent) is canceled, nullptr while it is not
        const char *message() const noexcept;

        Token(const Token &) = delete;
        Token &operator=(const Token &) = delete;

    private:
        explicit Token(Token *parent) noexcept;
        ~Token();

        std::atomic<int> refs_{1};
        std::atomic<const char *> message_{nullptr};
        Token *const parent_; // a reference is held on it
    };

    // A counted reference to a token (or none)
    class TokenRef
    {
    public:
        TokenRef() noexcept = default;
        // Takes a new reference to token
        explicit TokenRef(Token *token) noexcept;
        TokenRef(const TokenRef &other) noexcept;
        TokenRef(TokenRef &&other) noexcept;
        TokenRef &operator=(TokenRef other) noexcept;
        ~TokenRef();

        // Takes over the reference the caller holds
        static TokenRef adopt(Token *token) noexcept;

        Token *get() const noexcept { return token_; }
        explicit operator bool() const noexcept { return token_ != nullptr; }

    private:
        Token *token_ = nullptr;
    };

    // ---- the token of the calling thread

    // Sets the token the calling thread works for (a reference is held), or none (nullptr).
    // The C API resets it when the outermost call of the thread returns.
    void setThreadToken(Token *token) noexcept;
    TokenRef threadToken() noexcept;
    bool hasThreadToken() noexcept;

    // Sets the thread's token while in scope, then puts back the previous one: a worker of a
    // parallel section works for the token of the thread that started the section
    class ScopedThreadToken
    {
    public:
        explicit ScopedThreadToken(const TokenRef &token) noexcept;
        ~ScopedThreadToken();
        ScopedThreadToken(const ScopedThreadToken &) = delete;
        ScopedThreadToken &operator=(const ScopedThreadToken &) = delete;

    private:
        TokenRef previous_;
    };

    // A token for one call, child of the thread's token and made the thread's token while in
    // scope: the work of the call stops when the caller's token is canceled, the shutdown starts
    // or cancel() is called (by a progress callback that asks to stop).
    class CallToken
    {
    public:
        CallToken();
        void cancel(const char *message) noexcept { token_.get()->cancel(message); }

    private:
        TokenRef token_;
        ScopedThreadToken scope_;
    };

    // ---- the shutdown (DDBShutdown)

    // Stops the work of every thread, from now on
    void requestShutdown() noexcept;
    bool shutdownRequested() noexcept;
    // Unit tests only (with capi::resetShutdownForTests)
    void resetShutdownForTests() noexcept;

    // ---- checks

    // True when the work of the calling thread must stop: the shutdown started or its token is
    // canceled
    bool isCanceled() noexcept;
    // The same for the work of token, from any thread (GDAL calls progress callbacks from its
    // own worker threads)
    bool isCanceled(const Token *token) noexcept;
    // Throws ShutdownException or CanceledException (with the token's message) when the work of
    // the calling thread must stop
    void throwIfCanceled();

    // ---- GDAL

    // A GDAL progress callback (GDALProgressFunc) that stops the GDAL call when the work of the
    // thread that made it must stop. The token is taken when it is constructed, so GDAL may call
    // it from its own threads. Keep it alive while GDAL uses it (until the call returns).
    // GDAL then fails the call ("User terminated"): check throwIfCanceled() before reporting the
    // failure as an error.
    class GdalProgress
    {
    public:
        // Called with every progress fraction before the check (it must not throw)
        using Observer = std::function<void(double fraction)>;

        explicit GdalProgress(Observer observer = {});

        static int CPL_STDCALL callback(double complete, const char *message, void *self);
        GDALProgressFunc function() const noexcept { return &callback; }
        void *data() noexcept { return this; }

        void attach(GDALWarpAppOptions *options) noexcept;
        void attach(GDALTranslateOptions *options) noexcept;
        void attach(GDALVectorTranslateOptions *options) noexcept;
        // The extra argument of GDALRasterIOEx/GDALDatasetRasterIOEx with this progress
        GDALRasterIOExtraArg rasterIOExtraArg(GDALRIOResampleAlg resampling = GRIORA_NearestNeighbour) noexcept;

    private:
        TokenRef token_;
        Observer observer_;
    };

    // ---- SQLite

    // Installs the progress handler that interrupts a statement (SQLITE_INTERRUPT) when the work
    // of the thread running it must stop. It reads the running thread's state, never an argument,
    // so a pooled connection is safe whichever thread uses it next. Not while an exception
    // unwinds: the rollback of a canceled transaction runs.
    void installSqliteProgressHandler(sqlite3 *db) noexcept;

} // namespace ddb::cancel

#endif // CANCELLATION_H
