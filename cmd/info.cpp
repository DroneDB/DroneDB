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
#include "info.h"
#include "../libs/info.h"
#include "../classes/exceptions.h"

namespace cmd {

void Info::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args]")
    .custom_help("info *.JPG")
    .add_options()
    ("i,input", "File(s) to examine", cxxopts::value<std::vector<std::string>>())
    ("f,format", "Output format (text|json|geojson)", cxxopts::value<std::string>()->default_value("text"))
    ("r,recursive", "Recursively search in subdirectories", cxxopts::value<bool>())
    ("d,depth", "Max recursion depth", cxxopts::value<int>()->default_value("0"))
    ("with-hash", "Compute SHA256 hashes", cxxopts::value<bool>());
    opts.parse_positional({"input"});
}

std::string Info::description() {
    return "Retrieve information about files and directories";
}

void Info::run(cxxopts::ParseResult &opts) {
    if (!opts.count("input")) {
        printHelp();
    }

    auto input = opts["input"].as<std::vector<std::string>>();

    try{
        entry::ParseEntryOpts peOpts;
        peOpts.withHash = opts["with-hash"].count();

        ddb::ParseFilesOpts pfOpts;
        pfOpts.format = opts["format"].as<std::string>();
        pfOpts.recursive = opts["recursive"].count();
        pfOpts.maxRecursionDepth = opts["depth"].as<int>();
        pfOpts.peOpts = peOpts;

        ddb::parseFiles(input, std::cout, pfOpts);
    }catch(ddb::InvalidArgsException){
        printHelp();
    }
}

}


