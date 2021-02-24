/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "clone.h"

#include <constants.h>
#include <ddb.h>
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
			.custom_help("clone (tag|url)")
			.add_options()
			("t,target", "Repository tag or full url", cxxopts::value<std::string>())
			("f,folder", "Target folder", cxxopts::value<std::string>());

    // clang-format on
    opts.parse_positional({"target"});
}

std::string Clone::description() {
    return "Clone a repository into a new directory";
}

std::string Clone::extendedDescription() {
    return "\r\n\r\nClones a repository into a newly created directory.";
}

TargetInfo Clone::getCloneTarget(std::string& target) {
    TargetInfo info;

    if (target.find("http://") == std::string::npos &&
        target.find("https://") == std::string::npos) {
        // We assume it is not a link
        const auto needle = target.find('/');

        if (needle == std::string::npos)
            throw ddb::InvalidArgsException(
                "Tag format expected: organization/dataset");

        const int slashesCount = std::count(target.begin(), target.end(), '/');

        if (slashesCount > 1)
            throw ddb::InvalidArgsException(
                "Tag format expected: organization/dataset");

        info.dataset = target.substr(needle);
        info.organization = target.substr(0, needle);
        info.url = std::string("https://") + DEFAULT_REGISTRY + "/orgs/" +
                   info.organization + "/ds/" + info.dataset + "/download";

    } else {
        const homer6::Url u(target);

        const auto path = u.getPath();
        // https://testhub.dronedb.app/orgs/HeDo88/ds/cascina/download

        const std::regex e("orgs/(org)/ds/(ds)/download");

        std::smatch sm;
        std::regex_match(path, sm, e);
        std::cout << "string object with " << sm.size() << " matches\n";

        if (sm.empty())
            throw ddb::InvalidArgsException("Unexpected registry url format");

        info.dataset = sm[1].str();
        info.organization = sm[0].str();

        info.url = target;
    }

    info.folder = info.dataset;

    return info;
}

void Clone::run(cxxopts::ParseResult& opts) {
    try {
        if (opts["target"].count() != 1) {
            printHelp();
            return;
        }
        auto rawTarget = opts["target"].as<std::string>();

        const auto target = getCloneTarget(rawTarget);

        LOGD << "Target: " << target.url;
        LOGD << "Folder: " << target.folder;

        ddb::clone(target.url, opts["folder"].count() != 1
                                   ? opts["folder"].as<std::string>()
                                   : target.folder);

    } catch (ddb::InvalidArgsException) {
        printHelp();
    }
}

}  // namespace cmd
