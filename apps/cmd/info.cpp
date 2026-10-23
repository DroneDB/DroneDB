/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include <fstream>
#include <info.h>
#include "include/info.h"
#include "exceptions.h"
#include "basicgeometry.h"
#include "dbops.h"
#include "mio.h"

namespace cmd
{

    void Info::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("info *.JPG")
    .add_options()
    ("i,input", "File(s) to examine", cxxopts::value<std::vector<std::string>>());
    cmd::addOutputOption(opts, "Output file to write results to", "stdout");
    cmd::addFormatOption(opts, "text|json|geojson");
    opts.add_options()
    ("geometry", "Geometry to output (for geojson format only) (auto|point|polygon)", cxxopts::value<std::string>()->default_value("auto"))
    ("with-hash", "Compute SHA256 hashes", cxxopts::value<bool>());
        // clang-format on
        opts.parse_positional({"input"});
    }

    std::string Info::description()
    {
        return "Retrieve information about files and directories";
    }

    int Info::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("input"))
        {
            usageError("missing required argument 'input'");
        }

        auto input = opts["input"].as<std::vector<std::string>>();

        bool withHash = opts["with-hash"].count() > 0;
        const auto format = cmd::getFormatOption(
            opts, {ddb::OutputFormat::Text, ddb::OutputFormat::Json, ddb::OutputFormat::GeoJson});
        auto geometry = opts["geometry"].as<std::string>();

        // Always recursive for symmetry with `list`. Use glob patterns to scope.
        const bool recursive = true;
        const int maxRecursionDepth = 0;

        // Expand glob patterns to actual file paths.
        const auto expanded = ddb::expandGlobPatterns(input);

        if (opts.count("output"))
        {
            std::string filename = opts["output"].as<std::string>();
            std::ofstream file(ddb::io::fromUtf8(filename), std::ios::out | std::ios::trunc | std::ios::binary);
            if (!file.is_open())
                throw ddb::FSException("Cannot open " + filename);

            ddb::info(expanded, file, format, recursive, maxRecursionDepth,
                      geometry, withHash, !recursive);

            file.close();
        }
        else
        {
            ddb::info(expanded, std::cout, format, recursive, maxRecursionDepth,
                      geometry, withHash, !recursive);
        }

        return EXIT_SUCCESS;
    }

}
