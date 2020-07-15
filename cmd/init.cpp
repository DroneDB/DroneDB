/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "../fs.h"
#include "init.h"
#include "../libs/ddb.h"

namespace cmd {

void Init::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args] [DIRECTORY]")
    .custom_help("init")
    .add_options()
    ("d,directory", "Working directory", cxxopts::value<std::string>()->default_value("."));

    opts.parse_positional({"directory"});
}

std::string Init::description() {
    return "Initialize an index. If a directory is not specified, initializes the index in the current directory";
}

void Init::run(cxxopts::ParseResult &opts) {
    std::string p = ddb::create(opts["directory"].as<std::string>());
    std::cout << "Initialized empty database in " << p << std::endl;
}

}


