/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "push.h"

#include <constants.h>
#include <ddb.h>
#include <mio.h>
#include <registryutils.h>
#include <url/Url.h>

#include <fstream>
#include <iostream>

#include "dbops.h"
#include "exceptions.h"

namespace cmd {
void Push::setOptions(cxxopts::Options& opts) {
    // clang-format off
        opts
            .positional_help("[args]")
            .custom_help("push [remote]")
            .add_options()
                ("r,remote", "The remote Registry", cxxopts::value<std::string>()->default_value(""))
                ("f,force", "Forces the operation", cxxopts::value<bool>()->default_value("false"));


    // clang-format on
    opts.parse_positional({"remote"});
}

std::string Push::description() {
    return "Pushes local changes to remote repository.";
}

void Push::run(cxxopts::ParseResult& opts) {
    try {

        const auto force = opts["force"].as<bool>();
        const auto remote = opts["remote"].as<std::string>();

        ddb::push(remote, force);

    } catch (ddb::InvalidArgsException) {
        printHelp();
    }
}

}  // namespace cmd
