/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "mask.h"
#include "include/mask.h"
#include "exceptions.h"

namespace cmd
{

    void Mask::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("mask input.tif [-o output.tif] [-n 15] [-w] [-c r,g,b]")
    .add_options()
    ("i,input", "Input GeoTIFF file", cxxopts::value<std::string>())
    ("o,output", "Output GeoTIFF file (default: <input>_masked.tif)", cxxopts::value<std::string>()->default_value(""))
    ("n,near", "Tolerance in grey levels", cxxopts::value<int>()->default_value("15"))
    ("w,white", "Search for white borders instead of black", cxxopts::value<bool>()->default_value("false"))
    ("c,color", "Custom color r,g,b (e.g. 0,0,0)", cxxopts::value<std::string>()->default_value(""));

        // clang-format on
        opts.parse_positional({"input"});
    }

    std::string Mask::description()
    {
        return "Mask orthophoto borders making them transparent.";
    }

    void Mask::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("input"))
        {
            printHelp();
        }

        auto input = opts["input"].as<std::string>();
        auto output = opts["output"].as<std::string>();
        auto near = opts["near"].as<int>();
        auto white = opts["white"].as<bool>();
        auto color = opts["color"].as<std::string>();

        if (output.empty()) {
            // Generate default output name: <name>_masked.tif
            auto dotPos = input.rfind('.');
            if (dotPos != std::string::npos) {
                output = input.substr(0, dotPos) + "_masked.tif";
            } else {
                output = input + "_masked.tif";
            }
        }

        ddb::maskBorders(input, output, near, white, color);
        std::cout << "Output: " << output << std::endl;
    }

}
