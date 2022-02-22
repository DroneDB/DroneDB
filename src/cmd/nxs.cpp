/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "nxs.h"

#include <iostream>

#include "3d.h"

namespace cmd {

void Nxs::setOptions(cxxopts::Options &opts) {
    // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("nxs model.obj [output.nxz|output.nxs]")
    .add_options()
    ("i,input", "File to process", cxxopts::value<std::string>())
    ("o,output", "Nexus output file", cxxopts::value<std::string>())
    ("overwrite", "Overwrite output file if it exists", cxxopts::value<bool>()->default_value("false"));
    // clang-format on
    opts.parse_positional({"input", "output"});
}

std::string Nxs::description() {
    return "Generate nexus (NXS/NXZ) files from OBJs.";
}

void Nxs::run(cxxopts::ParseResult &opts) {
    if (!opts.count("input")) {
        printHelp();
    }

    const auto input = opts["input"].as<std::string>();
    std::string output = "";
    if (opts.count("output")) output = opts["output"].as<std::string>();

    std::cout << ddb::buildObj(input, output, opts["overwrite"].as<bool>()) << std::endl;
}

}  // namespace cmd
