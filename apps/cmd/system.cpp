/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "include/system.h"
#include "thumbs.h"
#include "tilerhelper.h"

namespace cmd
{

    void System::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("COMMAND")
    .custom_help("system")
    .add_options()
    ("c,command", "Command", cxxopts::value<std::string>()->default_value(""));
        // clang-format on
        opts.parse_positional({"command"});
    }

    std::string System::description()
    {
        return "Manage ddb";
    }

    std::string System::extendedDescription()
    {
        return "\r\n\r\nCommands:\r\n"
               "\tclean\tCleanup user cache files\r\n";
    }

    void System::run(cxxopts::ParseResult &opts)
    {
        auto cmd = opts["command"].as<std::string>();
        if (cmd == "clean")
        {
            ddb::TilerHelper::cleanupUserCache();
            ddb::cleanupThumbsUserCache();
        }
        else
        {
            printHelp();
        }
    }

}
