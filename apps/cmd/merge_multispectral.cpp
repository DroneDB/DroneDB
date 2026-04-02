/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "merge_multispectral.h"
#include "include/merge_multispectral.h"
#include "exceptions.h"
#include "json.h"

namespace cmd
{

    void MergeMultispectral::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("merge-multispectral -o output.tif band1.tif band2.tif ...")
    .add_options()
    ("o,output", "Output Cloud Optimized GeoTIFF path", cxxopts::value<std::string>())
    ("validate", "Only validate inputs, don't merge")
    ("i,input", "Input single-band raster files", cxxopts::value<std::vector<std::string>>());

        // clang-format on
        opts.parse_positional({"input"});
    }

    std::string MergeMultispectral::description()
    {
        return "Merge single-band raster files into a multi-band Cloud Optimized GeoTIFF.";
    }

    void MergeMultispectral::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("input"))
        {
            printHelp();
        }

        auto inputs = opts["input"].as<std::vector<std::string>>();

        if (opts.count("validate"))
        {
            auto result = ddb::validateMergeMultispectral(inputs);
            json j;
            j["ok"] = result.ok;
            j["errors"] = result.errors;
            j["warnings"] = result.warnings;
            j["summary"] = {
                {"totalBands", result.summary.totalBands},
                {"dataType", result.summary.dataType},
                {"width", result.summary.width},
                {"height", result.summary.height}
            };
            std::cout << j.dump(2) << std::endl;
            return;
        }

        if (!opts.count("output"))
        {
            printHelp();
        }

        auto output = opts["output"].as<std::string>();
        ddb::mergeMultispectral(inputs, output);
        std::cout << output << std::endl;
    }

}
