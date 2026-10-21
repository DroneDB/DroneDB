/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "tool_locator.h"

#include <cstdlib>
#include "logger.h"
#include "mio.h"

namespace ddb
{
    namespace tools
    {
        namespace
        {
#ifdef _WIN32
            constexpr const char *kExeSuffix = ".exe";
            constexpr char kPathSep = ';';
#else
            constexpr const char *kExeSuffix = "";
            constexpr char kPathSep = ':';
#endif

            fs::path searchOnPath(const std::string &fileName)
            {
                const auto path = io::getEnvUtf8("PATH");
                if (!path || path->empty()) return {};

                const std::string &p = *path;
                size_t start = 0;
                while (start <= p.size())
                {
                    size_t end = p.find(kPathSep, start);
                    if (end == std::string::npos) end = p.size();
                    const std::string dir = p.substr(start, end - start);
                    if (!dir.empty())
                    {
                        const fs::path candidate = io::fromUtf8(dir) / io::fromUtf8(fileName);
                        if (isExecutableFile(candidate)) return candidate;
                    }
                    if (end == p.size()) break;
                    start = end + 1;
                }
                return {};
            }
        } // namespace

        bool isExecutableFile(const fs::path &p)
        {
            std::error_code ec;
            if (!fs::exists(p, ec) || ec) return false;
            if (!fs::is_regular_file(p, ec) || ec) return false;
#ifdef _WIN32
            return true;
#else
            const auto perms = fs::status(p, ec).permissions();
            if (ec) return false;
            using fs::perms;
            return (perms & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none;
#endif
        }

        fs::path findTool(const std::string &baseName, const char *envVar)
        {
            const std::string fileName = baseName + kExeSuffix;

            if (envVar != nullptr)
            {
                if (const auto override = io::getEnvUtf8(envVar); override && !override->empty())
                {
                    const fs::path p = io::fromUtf8(*override);
                    if (isExecutableFile(p))
                    {
                        LOGD << fileName << " found via " << envVar << " at " << io::toUtf8(p);
                        return p;
                    }
                    LOGD << envVar << "=" << *override << " is not an executable file; treating "
                         << fileName << " as unavailable";
                    return {};
                }
            }

            const fs::path exeDir = io::getExeFolderPath();
            if (!exeDir.empty())
            {
                if (const auto c = exeDir / io::fromUtf8(fileName); isExecutableFile(c)) return c;
                if (exeDir.has_parent_path())
                    if (const auto c = exeDir.parent_path() / "bin" / io::fromUtf8(fileName); isExecutableFile(c)) return c;
            }

            return searchOnPath(fileName);
        }
    }
}
