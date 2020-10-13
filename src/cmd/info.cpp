/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include <fstream>
#include "info.h"
#include "../info.h"
#include "exceptions.h"
#include "basicgeometry.h"

namespace cmd {

void Info::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args]")
    .custom_help("info *.JPG")
    .add_options()
    ("i,input", "File(s) to examine", cxxopts::value<std::vector<std::string>>())
    ("o,output", "Output file to write results to", cxxopts::value<std::string>()->default_value("stdout"))
    ("f,format", "Output format (text|json|geojson)", cxxopts::value<std::string>()->default_value("text"))
    ("r,recursive", "Recursively search in subdirectories", cxxopts::value<bool>())
    ("d,depth", "Max recursion depth", cxxopts::value<int>()->default_value("0"))
    ("geometry", "Geometry to output (for geojson format only) (auto|point|polygon)", cxxopts::value<std::string>()->default_value("auto"))
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
        bool withHash = opts["with-hash"].count() > 0;
        auto format = opts["format"].as<std::string>();
        auto recursive = opts["recursive"].count() > 0;
        auto maxRecursionDepth = opts["depth"].as<int>();
        auto geometry = opts["geometry"].as<std::string>();

        if (opts.count("output")){
            std::string filename = opts["output"].as<std::string>();
            std::ofstream file(filename, std::ios::out | std::ios::trunc | std::ios::binary);
            if (!file.is_open()) throw ddb::FSException("Cannot open " + filename);

            ddb::info(input, file, format, recursive, maxRecursionDepth,
                      geometry, withHash, true);

            file.close();
        }else{
            ddb::info(input, std::cout, format, recursive, maxRecursionDepth,
                      geometry, withHash, true);
        }
    }catch(ddb::InvalidArgsException){
        printHelp();
    }
}

}

