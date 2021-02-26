/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "clone.h"

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
void Clone::setOptions(cxxopts::Options& opts) {
    // clang-format off
        opts
            .positional_help("[args]")
            .custom_help("clone (tag|url) folder")
            .add_options()
            ("t,target", "Repository tag or full url", cxxopts::value<std::string>())
            ("f,folder", "Target folder", cxxopts::value<std::string>()->default_value(""));

    // clang-format on
    opts.parse_positional({"target", "folder"});
}

std::string Clone::description() {
    return "Clone a repository into a new directory";
}

std::string Clone::extendedDescription() {
    return "\r\n\r\nClones a repository into a newly created directory.";
}

void Clone::run(cxxopts::ParseResult& opts) {
    try {
        if (opts["target"].count() != 1) {
            printHelp();
            return;
        }

        const auto tag =
            ddb::RegistryUtils::parseTag(opts["target"].as<std::string>());

        const auto folderRaw = opts["folder"].as<std::string>();

        const auto folder = fs::absolute(folderRaw.length() > 0 ? folderRaw : tag.dataset);

        LOGD << "Normalized folder = " << folder.generic_string();

        clone(tag, folder.generic_string());

    } catch (ddb::InvalidArgsException) {
        printHelp();
    }
}

}  // namespace cmd
