/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "cancellation.h"

#include <exception>
#include <new>
#include <utility>

#include <sqlite3.h>

#include "exceptions.h"

namespace ddb::cancel
{
    namespace
    {
        std::atomic<bool> shutdown{false};

        // The token of this thread (a reference is held)
        thread_local TokenRef currentToken;

        // SQLite runs the progress handler every this many virtual machine instructions: a few
        // tens of microseconds of work
        constexpr int kSqliteProgressInstructions = 1000;

        int sqliteProgress(void * /* never used: the handler reads the running thread */)
        {
            return std::uncaught_exceptions() == 0 && isCanceled() ? 1 : 0;
        }
    }

    // ---- Token

    Token::Token(Token *parent) noexcept : parent_(parent)
    {
        if (parent_ != nullptr)
            parent_->retain();
    }

    Token::~Token()
    {
        if (parent_ != nullptr)
            parent_->release();
    }

    Token *Token::create(Token *parent)
    {
        return new Token(parent);
    }

    void Token::retain() noexcept
    {
        refs_.fetch_add(1, std::memory_order_relaxed);
    }

    void Token::release() noexcept
    {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete this;
    }

    void Token::cancel(const char *message) noexcept
    {
        const char *expected = nullptr;
        message_.compare_exchange_strong(expected, message != nullptr ? message : kCanceledByToken);
    }

    const char *Token::message() const noexcept
    {
        if (const char *own = message_.load(std::memory_order_acquire))
            return own;
        return parent_ != nullptr ? parent_->message() : nullptr;
    }

    // ---- TokenRef

    TokenRef::TokenRef(Token *token) noexcept : token_(token)
    {
        if (token_ != nullptr)
            token_->retain();
    }

    TokenRef::TokenRef(const TokenRef &other) noexcept : TokenRef(other.token_) {}

    TokenRef::TokenRef(TokenRef &&other) noexcept : token_(std::exchange(other.token_, nullptr)) {}

    TokenRef &TokenRef::operator=(TokenRef other) noexcept
    {
        std::swap(token_, other.token_);
        return *this;
    }

    TokenRef::~TokenRef()
    {
        if (token_ != nullptr)
            token_->release();
    }

    TokenRef TokenRef::adopt(Token *token) noexcept
    {
        TokenRef ref;
        ref.token_ = token;
        return ref;
    }

    // ---- the thread's token

    void setThreadToken(Token *token) noexcept
    {
        currentToken = TokenRef(token);
    }

    TokenRef threadToken() noexcept
    {
        return currentToken;
    }

    bool hasThreadToken() noexcept
    {
        return static_cast<bool>(currentToken);
    }

    ScopedThreadToken::ScopedThreadToken(const TokenRef &token) noexcept : previous_(std::move(currentToken))
    {
        currentToken = token;
    }

    ScopedThreadToken::~ScopedThreadToken()
    {
        currentToken = std::move(previous_);
    }

    CallToken::CallToken() : token_(TokenRef::adopt(Token::create(currentToken.get()))), scope_(token_) {}

    // ---- shutdown

    void requestShutdown() noexcept
    {
        shutdown.store(true);
    }

    bool shutdownRequested() noexcept
    {
        return shutdown.load(std::memory_order_relaxed);
    }

    void resetShutdownForTests() noexcept
    {
        shutdown.store(false);
    }

    // ---- checks

    bool isCanceled(const Token *token) noexcept
    {
        return shutdownRequested() || (token != nullptr && token->canceled());
    }

    bool isCanceled() noexcept
    {
        return isCanceled(currentToken.get());
    }

    void throwIfCanceled()
    {
        if (shutdownRequested())
            throw ShutdownException(kShuttingDown);
        if (const Token *token = currentToken.get())
            if (const char *message = token->message())
                throw CanceledException(message);
    }

    // ---- GDAL

    GdalProgress::GdalProgress(Observer observer) : token_(currentToken), observer_(std::move(observer)) {}

    int CPL_STDCALL GdalProgress::callback(double complete, const char * /* message */, void *self)
    {
        auto *progress = static_cast<GdalProgress *>(self);
        if (progress->observer_)
        {
            try
            {
                progress->observer_(complete);
            }
            catch (...)
            {
                return FALSE; // no exception may cross GDAL's frames: stop the call instead
            }
        }
        return isCanceled(progress->token_.get()) ? FALSE : TRUE;
    }

    void GdalProgress::attach(GDALWarpAppOptions *options) noexcept
    {
        GDALWarpAppOptionsSetProgress(options, function(), data());
    }

    void GdalProgress::attach(GDALTranslateOptions *options) noexcept
    {
        GDALTranslateOptionsSetProgress(options, function(), data());
    }

    void GdalProgress::attach(GDALVectorTranslateOptions *options) noexcept
    {
        GDALVectorTranslateOptionsSetProgress(options, function(), data());
    }

    GDALRasterIOExtraArg GdalProgress::rasterIOExtraArg(GDALRIOResampleAlg resampling) noexcept
    {
        GDALRasterIOExtraArg extra;
        INIT_RASTERIO_EXTRA_ARG(extra);
        extra.eResampleAlg = resampling;
        extra.pfnProgress = function();
        extra.pProgressData = data();
        return extra;
    }

    // ---- SQLite

    void installSqliteProgressHandler(sqlite3 *db) noexcept
    {
        sqlite3_progress_handler(db, kSqliteProgressInstructions, &sqliteProgress, nullptr);
    }

} // namespace ddb::cancel
