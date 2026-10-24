/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <stdexcept>
#include <vector>
#include <string>
#include <system_error>

namespace ddb
{

    class AppException : public std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };
    class DBException : public AppException
    {
        using AppException::AppException;
    };
    class SQLException : public DBException
    {
        using DBException::DBException;
    };
    // SQLITE_BUSY / SQLITE_LOCKED: transient contention, safe to retry (see RetryPolicy).
    class DBBusyException : public DBException
    {
        using DBException::DBException;
    };
    class FSException : public AppException
    {
    public:
        using AppException::AppException;
        FSException(const std::string &message, const std::error_code &code) :
            AppException(message), code_(code) {}
        const std::error_code &code() const noexcept { return code_; }
    private:
        std::error_code code_;
    };

    // Only these classified failures can preserve an existing pending build. The C API reports
    // the reason (DDBGetLastErrorReason, c_api_util.h): with DDBERR_EXCEPTION from the 1.x
    // exports (DDBBuild), with DDBERR_RETRYABLE from the ones new in 2.0.
    enum class RetryableBuildReason { LowDiskSpace, NoSpaceOnDevice, IdleTimeout };
    class RetryableBuildException : public AppException
    {
    public:
        RetryableBuildException(const std::string &message, RetryableBuildReason reason) :
            AppException(message), reason_(reason) {}
        RetryableBuildReason reason() const noexcept { return reason_; }
    private:
        RetryableBuildReason reason_;
    };
    class ZipException : public AppException
    {
        using AppException::AppException;
    };
    class TimezoneException : public AppException
    {
        using AppException::AppException;
    };
    class IndexException : public AppException
    {
        using AppException::AppException;
    };
    class InvalidArgsException : public AppException
    {
        using AppException::AppException;
    };
    // The requested entry is not in the index. The exports added in 2.0 return it as
    // DDBERR_NOTFOUND; the older ones keep returning DDBERR_EXCEPTION.
    class EntryNotFoundException : public AppException {
        using AppException::AppException;
    };
    class GDALException : public AppException
    {
        using AppException::AppException;
    };
    class PDALException : public AppException
    {
        using AppException::AppException;
    };
    class UntwineException : public AppException
    {
        using AppException::AppException;
    };
    class Obj2TilesException : public AppException
    {
        using AppException::AppException;
    };
    class NetException : public AppException
    {
        using AppException::AppException;
    };
    class URLException : public AppException
    {
        using AppException::AppException;
    };
    class AuthException : public AppException
    {
        using AppException::AppException;
    };
    class JSONException : public AppException
    {
        using AppException::AppException;
    };
    class RegistryException : public AppException
    {
        using AppException::AppException;
    };
    class RegistryNotFoundException : public RegistryException
    {
        using RegistryException::RegistryException;
    };
    class NoStampException : public RegistryException
    {
        using RegistryException::RegistryException;
    };
    class PullRequiredException : public RegistryException
    {
        using RegistryException::RegistryException;
    };
    class BuildDepMissingException : public AppException
    {
    public:
        explicit BuildDepMissingException(const std::string &message) :
            AppException(message) {}

        BuildDepMissingException(const std::string &message, const std::string &missingDep) :
            AppException(message) {
            missingDependencies.push_back(missingDep);
        }

        BuildDepMissingException(const std::string &message, const std::vector<std::string> &missingDeps) :
            AppException(message), missingDependencies(missingDeps) {}

        const std::vector<std::string>& getMissingDependencies() const {
            return missingDependencies;
        }

    private:
        std::vector<std::string> missingDependencies;
    };
    class NotImplementedException : public AppException
    {
        using AppException::AppException;
    };
    // A progress callback asked to stop: the C API returns DDBERR_CANCELED from the exports
    // listed as cancellable in DDBGetCapabilities()
    class CanceledException : public AppException
    {
        using AppException::AppException;
    };
    // The work stopped because the library is shutting down (DDBShutdown): DDBERR_SHUTDOWN
    class ShutdownException : public CanceledException
    {
        using CanceledException::CanceledException;
    };

    // Build lock specific exceptions
    class BuildLockException : public AppException
    {
        using AppException::AppException;
    };
    class BuildInProgressException : public BuildLockException
    {
        using BuildLockException::BuildLockException;
    };
    class BuildLockPermissionException : public BuildLockException
    {
        using BuildLockException::BuildLockException;
    };
    class BuildLockDiskFullException : public BuildLockException
    {
        using BuildLockException::BuildLockException;
    };
    class BuildLockDirectoryException : public BuildLockException
    {
        using BuildLockException::BuildLockException;
    };

}
#endif // EXCEPTIONS_H
