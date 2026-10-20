/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "fileversion.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace ddb
{
    std::string fileVersion(const fs::path &file)
    {
#ifdef _WIN32
        std::error_code ec;
        const auto size = fs::file_size(file, ec);
        if (ec)
            return "";
        const auto mtime = fs::last_write_time(file, ec);
        if (ec)
            return "";
        return std::to_string(mtime.time_since_epoch().count()) + ":" + std::to_string(size);
#else
        struct stat st{};
        if (stat(file.c_str(), &st) != 0)
            return "";
#ifdef __APPLE__
        const auto &mtime = st.st_mtimespec;
#else
        const auto &mtime = st.st_mtim;
#endif
        return std::to_string(static_cast<long long>(mtime.tv_sec)) + "." +
               std::to_string(static_cast<long long>(mtime.tv_nsec)) + ":" +
               std::to_string(static_cast<unsigned long long>(st.st_size)) + ":" +
               std::to_string(static_cast<unsigned long long>(st.st_dev)) + ":" +
               std::to_string(static_cast<unsigned long long>(st.st_ino));
#endif
    }

    std::string fileIdentity(const fs::path &file)
    {
#ifdef _WIN32
        std::error_code ec;
        return fs::exists(file, ec) ? "file" : "";
#else
        struct stat st{};
        if (stat(file.c_str(), &st) != 0)
            return "";
        return std::to_string(static_cast<unsigned long long>(st.st_dev)) + ":" +
               std::to_string(static_cast<unsigned long long>(st.st_ino));
#endif
    }

    std::string cacheKeyPath(const fs::path &path)
    {
        // Canonical where the path exists (symlinks resolved and, on Windows, the letter
        // case of the file system), so two spellings of a folder release the same entries
        std::error_code ec;
        auto canonical = fs::weakly_canonical(path, ec);
        if (ec)
        {
            canonical = fs::absolute(path, ec);
            if (ec)
                canonical = path;
        }
        return canonical.lexically_normal().generic_string();
    }

    bool isCacheKeyUnder(const std::string &key, const std::string &prefix)
    {
        if (prefix.empty() || key.size() < prefix.size() || key.compare(0, prefix.size(), prefix) != 0)
            return false;
        return key.size() == prefix.size() || prefix.back() == '/' || key[prefix.size()] == '/';
    }
}
