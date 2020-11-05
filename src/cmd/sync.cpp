/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "sync.h"
#include "dbops.h"

namespace cmd {

void Sync::setOptions(cxxopts::Options &opts) {
    // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("sync")
    .add_options()
    ("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."));
    // clang-format on
}

std::string Sync::description() {
    return "Sync files and directories in the index with changes from the filesystem";
}

void Sync::run(cxxopts::ParseResult &opts) {

    auto workingDir = opts["working-dir"].as<std::string>();

    fs::current_path(workingDir);
    
    auto db = ddb::open(workingDir, true);
    ddb::syncIndex(db.get());
}

}


