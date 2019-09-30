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
#include "remove.h"
#include "../libs/ddb.h"

namespace fs = std::filesystem;

namespace cmd {

void Remove::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args] [PATHS]")
    .custom_help("rm")
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


