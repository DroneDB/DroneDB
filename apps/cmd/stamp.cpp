/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/stamp.h"
#include "dbops.h"

#include <iostream>

namespace cmd
{

    void Stamp::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("stamp");
    cmd::addWorkingDirOption(opts);
    cmd::addFormatOption(opts);
        // clang-format on
    }

    std::string Stamp::description()
    {
        return "Generate a stamp of the current index.";
    }

    int Stamp::run(cxxopts::ParseResult &opts)
    {
        const auto ddbPath = opts["working-dir"].as<std::string>();
        const auto format = opts["format"].as<std::string>();

        const auto db = ddb::open(std::string(ddbPath), true);
        output(std::cout, db->getStamp(), format);
        return EXIT_SUCCESS;
    }

} // namespace cmd
