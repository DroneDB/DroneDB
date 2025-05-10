/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <stdexcept>
#include <vector>
#include <string>

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
    class FSException : public AppException
    {
        using AppException::AppException;
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

}
#endif // EXCEPTIONS_H
