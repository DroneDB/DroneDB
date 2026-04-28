/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/add.h"

#include <iostream>

#include "dbops.h"
#include "fs.h"

namespace cmd
{

    void Add::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("[args] [PATHS]")
    .custom_help("add *.JPG")
    .add_options()
    ("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."))
    ("p,paths", "Paths or shell-style glob patterns to add to index (e.g. *.JPG, **/*.tif)", cxxopts::value<std::vector<std::string>>());
        // clang-format on
        opts.parse_positional({"paths"});
    }

    std::string Add::description()
    {
        return "Add files and directories to an index.";
    }

    void Add::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("paths"))
        {
            std::cerr << "error: missing required argument 'paths'" << std::endl
                      << std::endl;
            printHelp(std::cerr, false);
            exit(EXIT_FAILURE);
        }

        const auto ddbPath = opts["working-dir"].as<std::string>();
        const auto paths = opts["paths"].as<std::vector<std::string>>();

        const auto db = ddb::open(std::string(ddbPath), true);
        addToIndex(db.get(), ddb::expandGlobPatterns(paths),
                   [](const ddb::Entry &e, bool updated)
                   {
                       std::cout << (updated ? "U\t" : "A\t") << e.path
                                 << std::endl;
                       return true;
                   });
    }

} // namespace cmd
