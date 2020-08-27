/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "remove.h"
#include "ddb.h"

namespace cmd {

void Remove::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args] [PATHS]")
    .custom_help("rm image1.JPG image2.JPG [...]")
    .add_options()
    ("d,directory", "Working directory", cxxopts::value<std::string>()->default_value("."))
    ("r,recursive", "Recursively remove subdirectories and files", cxxopts::value<bool>())
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

    auto ddbPath = opts["directory"].as<std::string>();
    auto paths = opts["paths"].as<std::vector<std::string>>();

    std::vector<const char *> cPaths(paths.size());
    std::transform(paths.begin(), paths.end(), cPaths.begin(), [](const std::string& s) { return s.c_str(); });

    if (DDBRemove(ddbPath.c_str(), cPaths.data(), static_cast<int>(cPaths.size()), opts.count("recursive")) != DDBERR_NONE){
        std::cerr << DDBGetLastError() << std::endl;
    }
}

}


