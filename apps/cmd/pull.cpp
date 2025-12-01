/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/pull.h"

#include <authcredentials.h>
#include <constants.h>
#include <ddb.h>
#include <mio.h>
#include <registryutils.h>
#include <tagmanager.h>
#include <url.h>
#include <userprofile.h>

#include <fstream>
#include <iostream>

#include "dbops.h"
#include "exceptions.h"

namespace cmd
{
    void Pull::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
        opts
            .positional_help("[args]")
            .custom_help("pull")
            .add_options()
            ("r,remote", "The remote Registry", cxxopts::value<std::string>()->default_value(""))
            ("t,keep-theirs", "Keep changes from remote registry and override local ones", cxxopts::value<bool>()->default_value("false"))
            ("o,keep-ours", "Keep local changes override remote ones", cxxopts::value<bool>()->default_value("false"))
            ("k,insecure", "Disable SSL certificate verification", cxxopts::value<bool>());

        // clang-format on
        // opts.parse_positional({"remote"});
    }

    std::string Pull::description()
    {
        return "Pulls changes from a remote repository.";
    }

    void Pull::run(cxxopts::ParseResult &opts)
    {
        try
        {
            // const auto force = opts["force"].as<bool>();
            const auto keepTheirs = opts["keep-theirs"].as<bool>();
            const auto keepOurs = opts["keep-ours"].as<bool>();

            auto remote = opts["remote"].as<std::string>();
            auto sslVerify = opts["insecure"].count() == 0;

            ddb::MergeStrategy mergeStrategy = ddb::MergeStrategy::DontMerge;
            if (keepTheirs)
                mergeStrategy = ddb::MergeStrategy::KeepTheirs;
            else if (keepOurs)
                mergeStrategy = ddb::MergeStrategy::KeepOurs;

            ddb::pull(remote, mergeStrategy, sslVerify);
        }
        catch (ddb::IndexException &e)
        {
            std::cout << e.what() << std::endl;
        }
        catch (ddb::InvalidArgsException &ex)
        {
            std::cout << ex.what() << std::endl;
            printHelp();
        }
    }

} // namespace cmd
