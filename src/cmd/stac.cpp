/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "stac.h"
#include "../stac.h"
#include <iostream>

namespace cmd {

void Stac::setOptions(cxxopts::Options &opts) {
    // clang-format off
    opts
    .positional_help("[args] [PATHS]")
    .custom_help("stac")
    .add_options()
    ("p,paths", "DroneDB datasets to generate a STAC catalog for", cxxopts::value<std::vector<std::string>>()->default_value("."))
    ("m,match", "Metadata expression to match for a DroneDB dataset to be included in STAC catalog (in the form: key=value)", cxxopts::value<std::string>()->default_value(""))
    ("r,recursive", "Recursively scan for DroneDB datasets in path", cxxopts::value<bool>())
    ("d,depth", "Max recursion depth", cxxopts::value<int>()->default_value("2"));

    // clang-format on
    opts.parse_positional({"paths"});
}

std::string Stac::description() {
    return "Generate STAC catalogs";
}

void Stac::run(cxxopts::ParseResult &opts) {
    const auto paths = opts["paths"].as<std::vector<std::string>>();
    const auto matchExpr = opts["match"].as<std::string>();
    const bool recursive = opts["recursive"].as<bool>();
    const int maxRecursionDepth = opts["depth"].as<int>();

    std::cout << ddb::generateStac(paths, matchExpr, recursive, maxRecursionDepth) << std::endl;
}

}  // namespace cmd
