/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/gsplat.h"

#include <iostream>

#include "gsplat.h"
#include "mio.h"

namespace cmd
{

    void Gsplat::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("gsplat input.{ply|splat|spz} [output.spz]")
    .add_options()
    ("i,input", "Gaussian Splat file to convert (.ply, .splat or .spz)", cxxopts::value<std::string>())
    ("o,output", "Output .spz file", cxxopts::value<std::string>());
        // clang-format on
        opts.parse_positional({"input", "output"});
    }

    std::string Gsplat::description()
    {
        return "Convert a Gaussian Splat (.ply/.splat/.spz) to the compressed .spz format.";
    }

    void Gsplat::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("input"))
        {
            printHelp();
        }

        const auto input = opts["input"].as<std::string>();

        std::string output;
        if (opts.count("output"))
            output = opts["output"].as<std::string>();
        else
            output = fs::path(input).replace_extension(".spz").string();

        ddb::convertToSpz(input, output);
        std::cout << output << std::endl;
    }

} // namespace cmd
