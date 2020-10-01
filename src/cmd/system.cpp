/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "system.h"
#include "../thumbs.h"
#include "../tiler.h"

namespace cmd {

void System::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("COMMAND")
    .custom_help("system")
    .add_options()
    ("c,command", "Command", cxxopts::value<std::string>()->default_value(""));

    opts.parse_positional({"command"});
}

std::string System::description() {
    return "Manage ddb";
}

std::string System::extendedDescription(){
    return "\r\n\r\nCommands:\r\n"
           "\tclean\tCleanup user cache files\r\n";
}

void System::run(cxxopts::ParseResult &opts) {
    auto cmd = opts["command"].as<std::string>();
    if (cmd == "clean"){
        ddb::TilerHelper::cleanupUserCache();
        ddb::cleanupThumbsUserCache();
    }else{
        printHelp();
    }
}

}


