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
#include "build.h"
#include "../libs/ddb.h"
#include "../classes/database.h"
#include "../classes/exceptions.h"
#include "../logger.h"

namespace fs = std::filesystem;

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

    exit(1);
}

}


