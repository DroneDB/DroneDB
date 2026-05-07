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
        return "Remove orphaned build artifacts (build outputs whose source files no longer exist).";
    }

    void Cleanup::run(cxxopts::ParseResult &opts)
    {
        try
        {
            const auto ddbPath = opts["working-dir"].as<std::string>();

            const auto db = ddb::open(ddbPath, true);
            const auto removed = ddb::cleanupBuild(db.get(), "");

            if (removed.empty())
            {
                std::cout << "No orphaned build artifacts found." << std::endl;
            }
            else
            {
                for (const auto &p : removed)
                    std::cout << "Removed: " << p << std::endl;
                std::cout << removed.size() << " orphaned item(s) removed." << std::endl;
            }
        }
        catch (ddb::InvalidArgsException &)
        {
            printHelp();
        }
    }

} // namespace cmd
