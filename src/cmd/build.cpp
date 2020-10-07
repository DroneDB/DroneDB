/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "fs.h"
#include "build.h"
#include "dbops.h"

namespace cmd {

void Build::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args] [PATHS]")
    .custom_help("build")
    .add_options()
    ("paths", "Paths", cxxopts::value<std::vector<std::string>>());

    opts.parse_positional({"paths"});
}

std::string Build::description() {
    return "Initialize and build an index. Shorthand for running an init,add,commit command sequence.";
}

void Build::run(cxxopts::ParseResult &opts) {
    if (!opts.count("paths")) {
        printHelp();
    }

    std::cerr << "TODO: THIS MODULE IS A WORK IN PROGRESS!";

    exit(1);
}

}


