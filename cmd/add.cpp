/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include <iostream>
#include <filesystem>
#include "add.h"
#include "../libs/ddb.h"
#include "../classes/database.h"
#include "../classes/exceptions.h"
#include "../logger.h"
#include "../utils.h"

namespace fs = std::filesystem;

namespace cmd {

void Add::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args] [PATHS]")
    .custom_help("add")
    .add_options()
    ("d,directory", "Working directory", cxxopts::value<std::string>()->default_value("."))
    ("p,paths", "Paths to add to index (files or directories)", cxxopts::value<std::vector<std::string>>());

    opts.parse_positional({"paths"});
}

std::string Add::description() {
    return "Add files and directories to an index.";
}

void Add::run(cxxopts::ParseResult &opts) {
    if (!opts.count("paths")) {
        printHelp();
    }

    auto db = ddb::open(opts["directory"].as<std::string>(), true);
    ddb::addToIndex(db.get(), opts["paths"].as<std::vector<std::string>>());
}

}


