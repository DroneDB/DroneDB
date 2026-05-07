/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/cleanup.h"

#include <iostream>

#include "build.h"
#include "dbops.h"
#include "exceptions.h"
#include "fs.h"

namespace cmd
{

    void Cleanup::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
        opts
            .positional_help("[args]")
            .custom_help("cleanup [-w working-dir]")
            .add_options()
            ("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."));
        // clang-format on
    }

    std::string Cleanup::description()
    {
        return "Remove DB entries whose files no longer exist and orphaned build artifacts.";
    }

    void Cleanup::run(cxxopts::ParseResult &opts)
    {
        try
        {
            const auto ddbPath = opts["working-dir"].as<std::string>();

            const auto db = ddb::open(ddbPath, true);
            const auto result = ddb::cleanupBuild(db.get(), "");

            if (result.removedEntries.empty() && result.removedBuilds.empty())
            {
                std::cout << "Nothing to clean up." << std::endl;
                return;
            }

            if (!result.removedEntries.empty())
            {
                std::cout << "Removed stale DB entries:" << std::endl;
                for (const auto &p : result.removedEntries)
                    std::cout << "  - " << p << std::endl;
            }

            if (!result.removedBuilds.empty())
            {
                std::cout << "Removed orphan build artifacts:" << std::endl;
                for (const auto &p : result.removedBuilds)
                    std::cout << "  - " << p << std::endl;
            }

            std::cout << result.removedEntries.size() << " stale entry/entries, "
                      << result.removedBuilds.size() << " orphan build artifact(s) removed."
                      << std::endl;
        }
        catch (ddb::InvalidArgsException &)
        {
            printHelp();
        }
    }

} // namespace cmd
