/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "include/copc.h"
#include "pointcloud.h"
#include "exceptions.h"

namespace cmd
{

    void Copc::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("copc outdir/ *.las")
    .add_options()
    ("o,output", "Output directory where to store the COPC file (cloud.copc.laz)", cxxopts::value<std::string>())
    ("i,input", "File(s) to process", cxxopts::value<std::vector<std::string>>());

        // clang-format on
        opts.parse_positional({"output", "input"});
    }

    std::string Copc::description()
    {
        return "Build a COPC (Cloud Optimized Point Cloud) file from point cloud inputs.";
    }

    void Copc::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("input") || !opts.count("output"))
        {
            printHelp();
        }

        auto input = opts["input"].as<std::vector<std::string>>();
        auto output = opts["output"].as<std::string>();

        ddb::buildCopc(input, output);
    }

}
