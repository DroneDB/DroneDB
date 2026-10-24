/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "cog.h"
#include "include/cog.h"
#include "exceptions.h"

namespace cmd
{

    void Cog::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("cog cog.tif input.tif");
    cmd::addOutputOption(opts, "Output Cloud Optimized GeoTIFF");
    opts.add_options()
    ("i,input", "Input GeoTIFF to process", cxxopts::value<std::string>());

        // clang-format on
        opts.parse_positional({"output", "input"});
    }

    std::string Cog::description()
    {
        return "Build a Cloud Optimized GeoTIFF from an existing GeoTIFF.";
    }

    int Cog::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("input") || !opts.count("output"))
        {
            usageError();
        }

        auto input = opts["input"].as<std::string>();
        auto output = opts["output"].as<std::string>();

        ddb::buildCog(input, output);
        return EXIT_SUCCESS;
    }

}
