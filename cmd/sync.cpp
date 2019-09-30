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
#include "sync.h"
#include "../libs/ddb.h"


namespace fs = std::filesystem;

namespace cmd {

void Sync::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args]")
    .custom_help("sync")
    .add_options()
    ("d,directory", "Working directory", cxxopts::value<std::string>()->default_value("."));
}

std::string Sync::description() {
    return "Sync files and directories in the index with changes from the filesystem";
}

void Sync::run(cxxopts::ParseResult &opts) {
    auto db = ddb::open(opts["directory"].as<std::string>(), true);
    ddb::syncIndex(db.get());
}

}


