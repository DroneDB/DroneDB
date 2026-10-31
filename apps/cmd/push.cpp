/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/push.h"

#include <constants.h>
#include <ddb.h>
#include <mio.h>
#include <registry_ops.h>
#include <registryutils.h>

#include <fstream>
#include <iostream>

#include "dbops.h"
#include "exceptions.h"
#include "include/credentials.h"

namespace cmd
{
    void Push::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
        opts
            .positional_help("[args]")
            .custom_help("push [remote]");
        cmd::addRemoteOption(opts);
        cmd::addInsecureOption(opts);

        // clang-format on
        opts.parse_positional({"remote"});
    }

    std::string Push::description()
    {
        return "Pushes changes to a remote repository.";
    }

    int Push::run(cxxopts::ParseResult &opts)
    {
        // Errors reach Command::run: stderr and a non-zero exit (invalid args also print usage)
        const auto remote = opts["remote"].as<std::string>();
        auto sslVerify = opts["insecure"].count() == 0;

        ddb::push(remote, sslVerify, credentials(), std::cout, registryDeps());
        return EXIT_SUCCESS;
    }

} // namespace cmd
