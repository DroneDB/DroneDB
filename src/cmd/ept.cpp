/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "ept.h"
#include "pointcloud.h"
#include "exceptions.h"

namespace cmd {

void Ept::setOptions(cxxopts::Options &opts) {
    // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("ept outdir/ *.las")
    .add_options()
    ("o,output", "Output directory where to store EPT data", cxxopts::value<std::string>())
    ("i,input", "File(s) to process", cxxopts::value<std::vector<std::string>>());

    // clang-format on
    opts.parse_positional({"output", "input"});
}

std::string Ept::description() {
    return "Build an EPT index from point cloud files.";
}

void Ept::run(cxxopts::ParseResult &opts) {
    if (!opts.count("input") || !opts.count("output")) {
        printHelp();
    }

    auto input = opts["input"].as<std::vector<std::string>>();
    auto output = opts["output"].as<std::string>();

    ddb::buildEpt(input, output);
}

}


