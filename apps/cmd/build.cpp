/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"
#include <iostream>
#include "include/build.h"
#include <ddb.h>
#include "dbops.h"
#include "fs.h"

namespace cmd
{

    void Build::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
        .positional_help("[args]")
        .custom_help("build [-p path/to/file.laz] [--output out_dir]")
        .add_options()
        ("o,output", "Output folder", cxxopts::value<std::string>()->default_value((fs::path(DDB_FOLDER) / DDB_BUILD_PATH).string()))
        ("p,path", "File to process", cxxopts::value<std::string>())
    	("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."))
    	("f,force", "Force rebuild", cxxopts::value<bool>()->default_value("false"));

        // clang-format on
        opts.parse_positional({"path"});
    }

    std::string Build::description()
    {
        return "Build DroneDB files for efficient streaming over a network.";
    }

    void Build::run(cxxopts::ParseResult &opts)
    {

        try
        {

            const auto output = opts["output"].as<std::string>();
            const auto ddbPath = opts["working-dir"].as<std::string>();
            const auto force = opts["force"].as<bool>();

            if (output.length() == 0)
                printHelp();

            const auto db = ddb::open(ddbPath, true);

            if (!opts.count("path"))
            {
                buildAll(db.get(), output, force);
            }
            else
            {
                const auto path = opts["path"].as<std::string>();
                build(db.get(), path, output, force);
            }
        }
        catch (ddb::InvalidArgsException)
        {
            printHelp();
        }
    }

} // namespace cmd
