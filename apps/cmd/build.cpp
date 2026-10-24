/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"
#include <iostream>
#include "include/build.h"
#include <ddb.h>
#include "dbops.h"
#include "fs.h"
#include "mio.h"

namespace cmd
{

    void Build::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
        .positional_help("[args]")
        .custom_help("build [-p path/to/file.laz] [--output out_dir]");
    cmd::addOutputOption(opts, "Output folder", ddb::io::toUtf8(fs::path(DDB_FOLDER) / DDB_BUILD_PATH));
    opts.add_options()
        ("p,path", "File to process", cxxopts::value<std::string>());
    cmd::addWorkingDirOption(opts);
    opts.add_options()
    	("f,force", "Force rebuild", cxxopts::value<bool>()->default_value("false"));

        // clang-format on
        opts.parse_positional({"path"});
    }

    std::string Build::description()
    {
        return "Build DroneDB files for efficient streaming over a network.";
    }

    int Build::run(cxxopts::ParseResult &opts)
    {

        const auto output = opts["output"].as<std::string>();
        const auto ddbPath = opts["working-dir"].as<std::string>();
        const auto force = opts["force"].as<bool>();

        if (output.length() == 0)
            usageError();

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

        return EXIT_SUCCESS;
    }

} // namespace cmd
