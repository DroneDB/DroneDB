/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/push.h"

#include <constants.h>
#include <ddb.h>
#include <mio.h>
#include <registryutils.h>
#include <url.h>

#include <fstream>
#include <iostream>

#include "dbops.h"
#include "exceptions.h"

namespace cmd
{
    void Push::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
        opts
            .positional_help("[args]")
            .custom_help("push [remote]")
            .add_options()
                ("r,remote", "The remote Registry", cxxopts::value<std::string>()->default_value(""))
                ("k,insecure", "Disable SSL certificate verification", cxxopts::value<bool>());

        // clang-format on
        opts.parse_positional({"remote"});
    }

    std::string Push::description()
    {
        return "Pushes changes to a remote repository.";
    }

    void Push::run(cxxopts::ParseResult &opts)
    {
        try
        {
            const auto remote = opts["remote"].as<std::string>();
            auto sslVerify = opts["insecure"].count() == 0;

            ddb::push(remote, sslVerify);
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
