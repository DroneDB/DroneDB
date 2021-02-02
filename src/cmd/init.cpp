/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "fs.h"
#include "init.h"
#include "dbops.h"

namespace cmd {

void Init::setOptions(cxxopts::Options &opts) {
    // clang-format off
    opts
    .positional_help("[args] [DIRECTORY]")
    .custom_help("init")
    .add_options()
    ("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."))
    ("from-scratch", "Create the index database from scratch instead of using a prebuilt one (slower)", cxxopts::value<bool>());
    // clang-format on
    opts.parse_positional({"working-dir"});
}

std::string Init::description() {
    return "Initialize an index. If a directory is not specified, initializes the index in the current directory";
}

void Init::run(cxxopts::ParseResult &opts) {
    std::string p = opts["working-dir"].as<std::string>();
    bool fromScratch = opts["from-scratch"].count() > 0;

    std::string outPath = ddb::initIndex(p, fromScratch);
    std::cout << "Initialized empty database in " << outPath << std::endl;
}

}


