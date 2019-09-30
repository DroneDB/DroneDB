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
#include "init.h"
#include "../libs/ddb.h"

namespace fs = std::filesystem;

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


