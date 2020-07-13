/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "remove.h"
#include "../libs/ddb.h"


namespace cmd {

void Remove::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args] [PATHS]")
    .custom_help("rm image1.JPG image2.JPG [...]")
    .add_options()
    ("d,directory", "Working directory", cxxopts::value<std::string>()->default_value("."))
    ("p,paths", "Paths to remove from index (files or directories)", cxxopts::value<std::vector<std::string>>());

    opts.parse_positional({"paths"});
}

std::string Remove::description() {
    return "Remove files and directories from an index. The filesystem is left unchanged (actual files and directories will not be removed)";
}

void Remove::run(cxxopts::ParseResult &opts) {
    if (!opts.count("paths")) {
        printHelp();
    }

    auto db = ddb::open(opts["directory"].as<std::string>(), true);
    ddb::removeFromIndex(db.get(), opts["paths"].as<std::vector<std::string>>());
}

}


