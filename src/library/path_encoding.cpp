/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The path encoding helpers of mio.h ("Path encoding"): the only place where a std::string
// becomes an fs::path and back. No dependency beyond the standard library, so a program that
// compiles a library file on its own (tests/helpers/process_child.cpp with buildlock.cpp) can
// link it too.

#include "mio.h"

#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vector>
#endif

namespace ddb
{
    namespace io
    {

#ifdef _WIN32
        // MSVC's path keeps its wide string verbatim and converts char8_t strings as UTF-8
        // (std::system_error on an invalid sequence), whatever the code page or CRT locale
        fs::path fromUtf8(std::string_view utf8)
        {
            return fs::path(std::u8string(utf8.begin(), utf8.end()));
        }

        std::string toUtf8(const fs::path &p)
        {
            const std::u8string utf8 = p.u8string();
            return std::string(utf8.begin(), utf8.end());
        }

        std::string toGenericUtf8(const fs::path &p)
        {
            const std::u8string utf8 = p.generic_u8string();
            return std::string(utf8.begin(), utf8.end());
        }

        std::wstring toWide(std::string_view utf8)
        {
            return fromUtf8(utf8).native();
        }

        std::string fromWide(std::wstring_view wide)
        {
            return toUtf8(fs::path(wide));
        }

        // The process environment block (GetEnvironmentVariableW), not the CRT's copy that
        // _wgetenv reads: a host that is not C++ (the .NET Registry sets the PATH its tools are
        // found on with SetEnvironmentVariable) changes only the block, whereas _putenv_s
        // changes both, so this sees every change.
        std::optional<std::string> getEnvUtf8(const char *name)
        {
            const std::wstring wideName = toWide(name);
            std::vector<wchar_t> value(256);
            for (;;)
            {
                SetLastError(ERROR_SUCCESS);
                const DWORD length = GetEnvironmentVariableW(wideName.c_str(), value.data(),
                                                             static_cast<DWORD>(value.size()));
                if (length == 0)
                {
                    // 0 is an empty value or an unset variable
                    if (GetLastError() == ERROR_ENVVAR_NOT_FOUND)
                        return std::nullopt;
                    return std::string();
                }
                if (length < value.size())
                    return fromWide(std::wstring_view(value.data(), length));
                // Too small: length is the size needed, terminator included
                value.resize(length);
            }
        }
#else
        fs::path fromUtf8(std::string_view utf8)
        {
            return fs::path(std::string(utf8));
        }

        std::string toUtf8(const fs::path &p)
        {
            return p.native();
        }

        std::string toGenericUtf8(const fs::path &p)
        {
            return p.generic_string();
        }

        std::optional<std::string> getEnvUtf8(const char *name)
        {
            const char *value = std::getenv(name);
            if (value == nullptr)
                return std::nullopt;
            return std::string(value);
        }
#endif

        std::vector<fs::path> fromUtf8(const std::vector<std::string> &utf8)
        {
            std::vector<fs::path> paths;
            paths.reserve(utf8.size());
            for (const auto &s : utf8)
                paths.push_back(fromUtf8(s));
            return paths;
        }

        fs::path concat(const fs::path &p, std::string_view utf8)
        {
            fs::path result = p;
            result += fromUtf8(utf8);
            return result;
        }

    }
}
