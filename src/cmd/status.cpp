/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "status.h"
#include "../status.h"
#include "dbops.h"

namespace cmd {

void Status::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args]")
    .custom_help("status")
    .add_options()
    ("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."));
    
    // Nice to have
    // ("g,group", "Group by status", cxxopts::value<std::string>())
    // ("f,filter", "Filter entries", cxxopts::value<std::string>());
}

std::string Status::description() {
    return "Show files and directories in the index with changes from the filesystem and vice-versa";
}

void Status::run(cxxopts::ParseResult &opts) {

    const auto workingDir = opts["working-dir"].as<std::string>();
   
    const auto db = ddb::open(workingDir, true);

    statusIndex(db.get());
}

}


