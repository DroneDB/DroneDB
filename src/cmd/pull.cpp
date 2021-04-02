/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "pull.h"

#include <authcredentials.h>
#include <constants.h>
#include <ddb.h>
#include <mio.h>
#include <registryutils.h>
#include <tagmanager.h>
#include <url/Url.h>
#include <userprofile.h>

#include <fstream>
#include <iostream>

#include "dbops.h"
#include "exceptions.h"

namespace cmd {
void Pull::setOptions(cxxopts::Options& opts) {
    // clang-format off
        opts
            .positional_help("[args]")
            .custom_help("pull")
            .add_options()
            ("r,remote", "The remote Registry", cxxopts::value<std::string>()->default_value(""))
            ("f,force", "Forces the operation", cxxopts::value<bool>()->default_value("false"));

    // clang-format on
    //opts.parse_positional({"remote"});
}

std::string Pull::description() {
    return "Pulls changes from remote repository.";
}

void Pull::run(cxxopts::ParseResult& opts) {
    try {
        
        const auto force = opts["force"].as<bool>();
        auto remote = opts["remote"].as<std::string>();

        ddb::pull(remote, force);

    } catch (ddb::InvalidArgsException ex) {
        std::cout << ex.what() << std::endl;
        printHelp();
    }
}

}  // namespace cmd
